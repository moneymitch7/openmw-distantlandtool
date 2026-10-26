#include "texturebake.hpp"

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/vfs/pathutil.hpp>

#include <osg/Image>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace DistantLandTool
{
    namespace
    {
        struct Layer
        {
            std::string mPath;
            bool mHasAlpha = false;
            std::vector<std::uint8_t> mData; // every level, largest first
            bool mOk = false;
        };

        // The image shrunk (or grown) to size x size, box filtered: RGBA, 0..1.
        std::vector<float> sample(const osg::Image& image, int size)
        {
            const int w = std::max(1, image.s());
            const int h = std::max(1, image.t());
            std::vector<float> out(static_cast<std::size_t>(size) * size * 4, 0.f);
            for (int y = 0; y < size; ++y)
            {
                const int y0 = y * h / size;
                const int y1 = std::max(y0 + 1, (y + 1) * h / size);
                for (int x = 0; x < size; ++x)
                {
                    const int x0 = x * w / size;
                    const int x1 = std::max(x0 + 1, (x + 1) * w / size);
                    osg::Vec4 sum;
                    for (int sy = y0; sy < y1; ++sy)
                        for (int sx = x0; sx < x1; ++sx)
                            sum += image.getColor(static_cast<unsigned>(sx), static_cast<unsigned>(sy));
                    sum /= static_cast<float>((x1 - x0) * (y1 - y0));
                    float* p = &out[(static_cast<std::size_t>(y) * size + x) * 4];
                    for (int c = 0; c < 4; ++c)
                        p[c] = std::clamp(sum[c], 0.f, 1.f);
                }
            }
            return out;
        }

        std::vector<float> halve(const std::vector<float>& in, int size)
        {
            const int half = std::max(1, size / 2);
            std::vector<float> out(static_cast<std::size_t>(half) * half * 4, 0.f);
            for (int y = 0; y < half; ++y)
                for (int x = 0; x < half; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        float sum = 0.f;
                        int n = 0;
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                            {
                                const int sx = std::min(size - 1, x * 2 + dx);
                                const int sy = std::min(size - 1, y * 2 + dy);
                                sum += in[(static_cast<std::size_t>(sy) * size + sx) * 4 + c];
                                ++n;
                            }
                        out[(static_cast<std::size_t>(y) * half + x) * 4 + c] = sum / n;
                    }
            return out;
        }

        float coverage(const std::vector<float>& rgba, float ref, float scale)
        {
            std::size_t covered = 0;
            const std::size_t count = rgba.size() / 4;
            for (std::size_t i = 0; i < count; ++i)
                if (std::min(1.f, rgba[i * 4 + 3] * scale) >= ref)
                    ++covered;
            return count == 0 ? 0.f : static_cast<float>(covered) / count;
        }

        // Smaller levels average alpha, so an alpha-tested texture (leaves, fences) thins out and vanishes from afar:
        // scaled up so that as much of each level passes the test as of the largest. Only ever up: a texture whose
        // alpha mostly stays under the reference (soft leaves that a model tests with a lower threshold, which the
        // game keeps) has nothing to keep, and scaling it down would empty the smaller levels.
        void keepCoverage(std::vector<float>& rgba, float ref, float target)
        {
            if (target <= 0.f || coverage(rgba, ref, 1.f) >= target)
                return;
            float low = 1.f;
            float high = 8.f;
            for (int i = 0; i < 12; ++i)
            {
                const float mid = (low + high) / 2;
                if (coverage(rgba, ref, mid) < target)
                    low = mid;
                else
                    high = mid;
            }
            for (std::size_t i = 3; i < rgba.size(); i += 4)
                rgba[i] = std::min(1.f, rgba[i] * high);
        }

        std::uint16_t to565(const std::array<float, 3>& c)
        {
            const int r = std::clamp(static_cast<int>(std::lround(c[0] * 31.f / 255.f)), 0, 31);
            const int g = std::clamp(static_cast<int>(std::lround(c[1] * 63.f / 255.f)), 0, 63);
            const int b = std::clamp(static_cast<int>(std::lround(c[2] * 31.f / 255.f)), 0, 31);
            return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
        }

        std::array<float, 3> from565(std::uint16_t v)
        {
            const int r = (v >> 11) & 31;
            const int g = (v >> 5) & 63;
            const int b = v & 31;
            return { static_cast<float>((r << 3) | (r >> 2)), static_cast<float>((g << 2) | (g >> 4)),
                static_cast<float>((b << 3) | (b >> 2)) };
        }

        void encodeBlock(const std::array<std::array<float, 4>, 16>& px, std::uint8_t* out)
        {
            // alpha: the extremes and six steps between
            float amin = 255.f;
            float amax = 0.f;
            for (const auto& p : px)
            {
                amin = std::min(amin, p[3]);
                amax = std::max(amax, p[3]);
            }
            const std::uint8_t a0 = static_cast<std::uint8_t>(std::lround(amax));
            const std::uint8_t a1 = static_cast<std::uint8_t>(std::lround(amin));
            std::array<float, 8> alphas{};
            alphas[0] = a0;
            alphas[1] = a1;
            for (int i = 1; i <= 6; ++i)
                alphas[i + 1] = ((7 - i) * a0 + i * a1) / 7.f;
            std::uint64_t alphaBits = 0;
            if (a0 != a1)
                for (int i = 0; i < 16; ++i)
                {
                    int best = 0;
                    float bestError = 1e9f;
                    for (int k = 0; k < 8; ++k)
                    {
                        const float e = std::abs(alphas[k] - px[i][3]);
                        if (e < bestError)
                        {
                            bestError = e;
                            best = k;
                        }
                    }
                    alphaBits |= static_cast<std::uint64_t>(best) << (3 * i);
                }
            out[0] = a0;
            out[1] = a1;
            for (int i = 0; i < 6; ++i)
                out[2 + i] = static_cast<std::uint8_t>(alphaBits >> (8 * i));

            // colour: the extremes along the block's main direction of change (its principal axis), pulled in a
            // little, and two steps between
            std::array<float, 3> mean{ 0.f, 0.f, 0.f };
            for (const auto& p : px)
                for (int c = 0; c < 3; ++c)
                    mean[c] += p[c] / 16.f;
            std::array<std::array<float, 3>, 3> cov{};
            for (const auto& p : px)
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        cov[i][j] += (p[i] - mean[i]) * (p[j] - mean[j]);
            std::array<float, 3> axis{ 1.f, 1.f, 1.f };
            for (int iteration = 0; iteration < 8; ++iteration)
            {
                std::array<float, 3> next{};
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        next[i] += cov[i][j] * axis[j];
                const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
                if (length < 1e-6f)
                    break;
                for (int i = 0; i < 3; ++i)
                    axis[i] = next[i] / length;
            }
            float tmin = 1e9f;
            float tmax = -1e9f;
            for (const auto& p : px)
            {
                const float t = (p[0] - mean[0]) * axis[0] + (p[1] - mean[1]) * axis[1] + (p[2] - mean[2]) * axis[2];
                tmin = std::min(tmin, t);
                tmax = std::max(tmax, t);
            }
            const float inset = (tmax - tmin) / 32.f;
            tmin += inset;
            tmax -= inset;
            std::array<float, 3> lo{};
            std::array<float, 3> hi{};
            for (int c = 0; c < 3; ++c)
            {
                lo[c] = std::clamp(mean[c] + axis[c] * tmin, 0.f, 255.f);
                hi[c] = std::clamp(mean[c] + axis[c] * tmax, 0.f, 255.f);
            }
            std::uint16_t c0 = to565(hi);
            std::uint16_t c1 = to565(lo);
            if (c0 < c1)
                std::swap(c0, c1);
            const std::array<float, 3> e0 = from565(c0);
            const std::array<float, 3> e1 = from565(c1);
            std::array<std::array<float, 3>, 4> palette{};
            palette[0] = e0;
            palette[1] = e1;
            for (int c = 0; c < 3; ++c)
            {
                palette[2][c] = (2 * e0[c] + e1[c]) / 3.f;
                palette[3][c] = (e0[c] + 2 * e1[c]) / 3.f;
            }
            std::uint32_t colorBits = 0;
            if (c0 != c1)
                for (int i = 0; i < 16; ++i)
                {
                    int best = 0;
                    float bestError = 1e9f;
                    for (int k = 0; k < 4; ++k)
                    {
                        float e = 0.f;
                        for (int c = 0; c < 3; ++c)
                        {
                            const float d = palette[k][c] - px[i][c];
                            e += d * d;
                        }
                        if (e < bestError)
                        {
                            bestError = e;
                            best = k;
                        }
                    }
                    colorBits |= static_cast<std::uint32_t>(best) << (2 * i);
                }
            out[8] = static_cast<std::uint8_t>(c0 & 0xff);
            out[9] = static_cast<std::uint8_t>(c0 >> 8);
            out[10] = static_cast<std::uint8_t>(c1 & 0xff);
            out[11] = static_cast<std::uint8_t>(c1 >> 8);
            for (int i = 0; i < 4; ++i)
                out[12 + i] = static_cast<std::uint8_t>(colorBits >> (8 * i));
        }

        Layer bake(const std::string& path, Resource::ImageManager& images, const TextureBakeOptions& options)
        {
            Layer layer;
            layer.mPath = path;
            const osg::ref_ptr<osg::Image> image = images.getImage(VFS::Path::toNormalized(path));
            if (image == nullptr || image.get() == images.getWarningImage() || image->s() < 1 || image->t() < 1)
                return layer;
            const int size = options.mSize;
            std::vector<float> level = sample(*image, size);
            float baseCoverage = 1.f;
            for (std::size_t i = 3; i < level.size(); i += 4)
                if (level[i] < 0.98f)
                {
                    layer.mHasAlpha = true;
                    break;
                }
            if (layer.mHasAlpha)
                baseCoverage = coverage(level, options.mAlphaRef, 1.f);
            for (int s = size; s >= 1; s /= 2)
            {
                if (s != size)
                {
                    level = halve(level, s * 2);
                    if (layer.mHasAlpha)
                        keepCoverage(level, options.mAlphaRef, baseCoverage);
                }
                const std::vector<std::uint8_t> blocks = encodeDxt5(level, s, s);
                layer.mData.insert(layer.mData.end(), blocks.begin(), blocks.end());
            }
            layer.mOk = true;
            return layer;
        }

        template <class T>
        void put(std::ostream& stream, T value)
        {
            static_assert(std::endian::native == std::endian::little);
            stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
        }
    }

    std::vector<std::uint8_t> encodeDxt5(const std::vector<float>& rgba, int width, int height)
    {
        const int bw = (width + 3) / 4;
        const int bh = (height + 3) / 4;
        std::vector<std::uint8_t> out(static_cast<std::size_t>(bw) * bh * 16);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
            {
                std::array<std::array<float, 4>, 16> px{};
                for (int i = 0; i < 16; ++i)
                {
                    const int x = std::min(width - 1, bx * 4 + i % 4);
                    const int y = std::min(height - 1, by * 4 + i / 4);
                    const float* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
                    for (int c = 0; c < 4; ++c)
                        px[i][c] = std::clamp(p[c], 0.f, 1.f) * 255.f;
                }
                encodeBlock(px, &out[(static_cast<std::size_t>(by) * bw + bx) * 16]);
            }
        return out;
    }

    TextureBakeResult bakeTextures(const std::vector<std::string>& paths, Resource::ImageManager& images,
        std::ostream& stream, const TextureBakeOptions& options)
    {
        std::vector<Layer> layers(paths.size());
        std::atomic<std::size_t> next = 0;
        std::mutex logMutex;
        // a batch at a time, the image cache emptied between (the textures don't fit in memory together)
        constexpr std::size_t batchSize = 256;
        std::size_t start = 0;
        while (start < paths.size())
        {
            const std::size_t end = std::min(paths.size(), start + batchSize);
            std::vector<std::thread> threads;
            const std::size_t batchEnd = end;
            next = start;
            const auto batchWorker = [&] {
                while (true)
                {
                    const std::size_t i = next++;
                    if (i >= batchEnd)
                        return;
                    try
                    {
                        layers[i] = bake(paths[i], images, options);
                    }
                    catch (const std::exception& e)
                    {
                        const std::lock_guard lock(logMutex);
                        Log(Debug::Warning) << "Failed to bake " << paths[i] << ": " << e.what();
                    }
                }
            };
            for (std::size_t t = 0; t < std::max<std::size_t>(options.mThreads, 1); ++t)
                threads.emplace_back(batchWorker);
            for (std::thread& thread : threads)
                thread.join();
            images.clearCache();
            start = end;
            Log(Debug::Info) << "Baked " << end << " of " << paths.size() << " textures";
        }

        TextureBakeResult result;
        std::uint32_t levels = 0;
        for (int s = options.mSize; s >= 1; s /= 2)
            ++levels;
        std::vector<const Layer*> ok;
        for (const Layer& layer : layers)
        {
            if (layer.mOk)
                ok.push_back(&layer);
            else
                ++result.mFailed;
        }
        stream.write("OMWDLTX1", 8);
        put<std::uint32_t>(stream, 1);
        put<std::uint32_t>(stream, static_cast<std::uint32_t>(options.mSize));
        put<std::uint32_t>(stream, levels);
        put<std::uint32_t>(stream, static_cast<std::uint32_t>(ok.size()));
        put<std::uint32_t>(stream, textureBakeFormat);
        for (const Layer* layer : ok)
        {
            put<std::uint16_t>(stream, static_cast<std::uint16_t>(layer->mPath.size()));
            stream.write(layer->mPath.data(), static_cast<std::streamsize>(layer->mPath.size()));
            put<std::uint8_t>(stream, layer->mHasAlpha ? 1 : 0);
        }
        for (const Layer* layer : ok)
        {
            stream.write(
                reinterpret_cast<const char*>(layer->mData.data()), static_cast<std::streamsize>(layer->mData.size()));
            result.mBytes += layer->mData.size();
        }
        result.mLayers = ok.size();
        return result;
    }
}
