#ifndef OPENMW_APPS_DISTANTLANDTOOL_TEXTUREBAKE_H
#define OPENMW_APPS_DISTANTLANDTOOL_TEXTUREBAKE_H

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace Resource
{
    class ImageManager;
}

namespace DistantLandTool
{
    // The distant texture set (stage 2): small copies of the base textures of the models placed outside, all the same
    // size and compressed alike, so that the game can draw a distant chunk's objects from one texture array.
    //
    // File (little-endian), distantland/textures.bin in the output folder:
    //   char[8] "OMWDLTX1", uint32 version (1), uint32 size (of a layer, square), uint32 levels (size down to 1),
    //   uint32 layer count, uint32 format (0x83F3, DXT5 / BC3)
    //   per layer: uint16 path length, path (as the data folders name it: textures/...), uint8 flags (1: has alpha)
    //   then per layer, per level from the largest: the compressed blocks (ceil(w/4) * ceil(h/4) * 16 bytes)
    constexpr std::string_view textureBakeName = "distantland/textures.bin";
    constexpr std::uint32_t textureBakeFormat = 0x83F3; // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT

    struct TextureBakeOptions
    {
        int mSize = 128;
        // the alpha test threshold whose coverage the smaller levels keep (foliage stays as thick from afar)
        float mAlphaRef = 0.5f;
        std::size_t mThreads = 1;
    };

    struct TextureBakeResult
    {
        std::size_t mLayers = 0;
        std::size_t mFailed = 0;
        std::size_t mBytes = 0;
    };

    /// Reads, shrinks and compresses @a paths (textures/... in the data folders) and writes the set to @a stream.
    TextureBakeResult bakeTextures(const std::vector<std::string>& paths, Resource::ImageManager& images,
        std::ostream& stream, const TextureBakeOptions& options);

    /// Encodes RGBA (0..1, row-major, @a width x @a height) as DXT5 blocks. Exposed for the self test.
    std::vector<std::uint8_t> encodeDxt5(const std::vector<float>& rgba, int width, int height);
}

#endif
