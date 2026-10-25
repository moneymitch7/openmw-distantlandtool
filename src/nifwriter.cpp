#include "nifwriter.hpp"

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/BoundingSphere>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

namespace DistantLandTool
{
    namespace
    {
        // Little-endian binary output.
        class Out
        {
        public:
            void u8(std::uint8_t v) { mData.push_back(static_cast<char>(v)); }
            void u16(std::uint16_t v) { raw(&v, sizeof(v)); }
            void u32(std::uint32_t v) { raw(&v, sizeof(v)); }
            void i32(std::int32_t v) { raw(&v, sizeof(v)); }
            void f32(float v) { raw(&v, sizeof(v)); }
            // booleans are 4 bytes before NIF 4.1
            void boolean(bool v) { i32(v ? 1 : 0); }
            void string(const std::string& s)
            {
                u32(static_cast<std::uint32_t>(s.size()));
                mData.append(s);
            }
            void vec3(const osg::Vec3f& v)
            {
                f32(v.x());
                f32(v.y());
                f32(v.z());
            }
            void text(const std::string& s) { mData.append(s); }

            const std::string& data() const { return mData; }

        private:
            std::string mData;

            void raw(const void* p, std::size_t size)
            {
                static_assert(std::endian::native == std::endian::little, "NIF files are little-endian");
                mData.append(static_cast<const char*>(p), size);
            }
        };

        // NiAlphaProperty's codes for blend factors and test functions (as NifOsg::Loader reads them)
        std::uint16_t blendCode(int glFactor)
        {
            switch (glFactor)
            {
                case osg::BlendFunc::ONE:
                    return 0;
                case osg::BlendFunc::ZERO:
                    return 1;
                case osg::BlendFunc::SRC_COLOR:
                    return 2;
                case osg::BlendFunc::ONE_MINUS_SRC_COLOR:
                    return 3;
                case osg::BlendFunc::DST_COLOR:
                    return 4;
                case osg::BlendFunc::ONE_MINUS_DST_COLOR:
                    return 5;
                case osg::BlendFunc::SRC_ALPHA:
                    return 6;
                case osg::BlendFunc::ONE_MINUS_SRC_ALPHA:
                    return 7;
                case osg::BlendFunc::DST_ALPHA:
                    return 8;
                case osg::BlendFunc::ONE_MINUS_DST_ALPHA:
                    return 9;
                case osg::BlendFunc::SRC_ALPHA_SATURATE:
                    return 10;
                default:
                    return 6;
            }
        }

        std::uint16_t testCode(int function)
        {
            switch (function)
            {
                case osg::AlphaFunc::ALWAYS:
                    return 0;
                case osg::AlphaFunc::LESS:
                    return 1;
                case osg::AlphaFunc::EQUAL:
                    return 2;
                case osg::AlphaFunc::LEQUAL:
                    return 3;
                case osg::AlphaFunc::GREATER:
                    return 4;
                case osg::AlphaFunc::NOTEQUAL:
                    return 5;
                case osg::AlphaFunc::GEQUAL:
                    return 6;
                case osg::AlphaFunc::NEVER:
                    return 7;
                default:
                    return 6;
            }
        }

        // The blocks of the file, in order: each writes its type and data, links by index.
        struct Block
        {
            std::string mType;
            Out mData;
        };

        class Blocks
        {
        public:
            /// @return the index of the new block
            std::int32_t add(const std::string& type, Block*& block)
            {
                mBlocks.push_back(std::make_unique<Block>());
                block = mBlocks.back().get();
                block->mType = type;
                return static_cast<std::int32_t>(mBlocks.size() - 1);
            }

            std::size_t size() const { return mBlocks.size(); }
            const Block& operator[](std::size_t i) const { return *mBlocks[i]; }

        private:
            std::vector<std::unique_ptr<Block>> mBlocks;
        };

        void writeObjectNET(Out& out, const std::string& name)
        {
            out.string(name);
            out.i32(-1); // extra data
            out.i32(-1); // controller
        }

        void writeAVObject(Out& out, const std::string& name, const std::vector<std::int32_t>& properties)
        {
            writeObjectNET(out, name);
            out.u16(0); // flags
            out.vec3(osg::Vec3f()); // translation
            for (int row = 0; row < 3; ++row) // rotation
                for (int column = 0; column < 3; ++column)
                    out.f32(row == column ? 1.f : 0.f);
            out.f32(1.f); // scale
            out.vec3(osg::Vec3f()); // velocity
            out.u32(static_cast<std::uint32_t>(properties.size()));
            for (std::int32_t property : properties)
                out.i32(property);
            out.boolean(false); // no bounding volume
        }

        // Properties are shared between shapes that look alike (by what they hold).
        class Properties
        {
        public:
            explicit Properties(Blocks& blocks)
                : mBlocks(blocks)
            {
            }

            std::vector<std::int32_t> get(const DistantShape& shape)
            {
                std::vector<std::int32_t> result;
                if (!shape.mTexture.empty())
                    result.push_back(texturing(shape));
                if (shape.mHasMaterial)
                    result.push_back(material(shape));
                if (shape.mAlphaTest || shape.mBlend)
                    result.push_back(alpha(shape));
                if (!shape.mColors.empty())
                    result.push_back(vertexColor(shape));
                if (shape.mTwoSided)
                    result.push_back(twoSided());
                return result;
            }

        private:
            Blocks& mBlocks;
            std::unordered_map<std::string, std::int32_t> mShared;

            template <class F>
            std::int32_t shared(const std::string& key, F&& make)
            {
                const auto it = mShared.find(key);
                if (it != mShared.end())
                    return it->second;
                const std::int32_t index = make();
                mShared.emplace(key, index);
                return index;
            }

            std::int32_t source(const std::string& file)
            {
                return shared("source:" + file, [&] {
                    Block* block;
                    const std::int32_t index = mBlocks.add("NiSourceTexture", block);
                    Out& out = block->mData;
                    writeObjectNET(out, std::string());
                    out.u8(1); // external
                    out.string(file);
                    out.u32(5); // pixel layout: default
                    out.u32(2); // mipmaps: default
                    out.u32(3); // alpha: default
                    out.u8(1); // static
                    return index;
                });
            }

            std::int32_t texturing(const DistantShape& shape)
            {
                const std::uint32_t clamp = (shape.mWrapS ? 2u : 0u) | (shape.mWrapT ? 1u : 0u);
                const std::int32_t sourceIndex = source(shape.mTexture);
                return shared("texturing:" + std::to_string(sourceIndex) + ":" + std::to_string(clamp), [&] {
                    Block* block;
                    const std::int32_t index = mBlocks.add("NiTexturingProperty", block);
                    Out& out = block->mData;
                    writeObjectNET(out, std::string());
                    out.u16(0); // flags
                    out.u32(2); // apply mode: modulate
                    out.u32(7); // base, dark, detail, gloss, glow, bump, decal
                    for (int i = 0; i < 7; ++i)
                    {
                        out.boolean(i == 0);
                        if (i != 0)
                            continue;
                        out.i32(sourceIndex);
                        out.u32(clamp);
                        out.u32(2); // filter: trilinear
                        out.u32(0); // UV set
                        out.u16(0); // PS2 L
                        out.u16(0xffb5); // PS2 K
                        out.u16(0); // unknown
                    }
                    return index;
                });
            }

            std::int32_t material(const DistantShape& shape)
            {
                std::string key = "material:";
                for (const osg::Vec4f& v : { shape.mAmbient, shape.mDiffuse, shape.mSpecular, shape.mEmission })
                    key += std::to_string(v.x()) + "," + std::to_string(v.y()) + "," + std::to_string(v.z()) + ";";
                key += std::to_string(shape.mShininess) + ";" + std::to_string(shape.mDiffuse.a());
                return shared(key, [&] {
                    Block* block;
                    const std::int32_t index = mBlocks.add("NiMaterialProperty", block);
                    Out& out = block->mData;
                    writeObjectNET(out, std::string());
                    out.u16(0); // flags
                    const auto rgb = [](const osg::Vec4f& v) { return osg::Vec3f(v.x(), v.y(), v.z()); };
                    out.vec3(rgb(shape.mAmbient));
                    out.vec3(rgb(shape.mDiffuse));
                    out.vec3(rgb(shape.mSpecular));
                    out.vec3(rgb(shape.mEmission));
                    out.f32(shape.mShininess);
                    out.f32(shape.mDiffuse.a());
                    return index;
                });
            }

            std::int32_t alpha(const DistantShape& shape)
            {
                std::uint16_t flags = 0;
                if (shape.mBlend)
                    flags |= 1 | (blendCode(shape.mBlendSource) << 1) | (blendCode(shape.mBlendDestination) << 5);
                std::uint8_t threshold = 0;
                if (shape.mAlphaTest)
                {
                    flags |= (1 << 9) | (testCode(shape.mAlphaFunc) << 10);
                    threshold = static_cast<std::uint8_t>(std::clamp(shape.mAlphaRef * 255.f + 0.5f, 0.f, 255.f));
                }
                return shared("alpha:" + std::to_string(flags) + ":" + std::to_string(threshold), [&] {
                    Block* block;
                    const std::int32_t index = mBlocks.add("NiAlphaProperty", block);
                    Out& out = block->mData;
                    writeObjectNET(out, std::string());
                    out.u16(flags);
                    out.u8(threshold);
                    return index;
                });
            }

            std::int32_t vertexColor(const DistantShape& shape)
            {
                // SceneUtil::VertexColorModes: None 0, Emission 1, AmbientAndDiffuse 2 (and the others, which a
                // NIF can't say): as NiVertexColorProperty's source modes
                const std::uint32_t mode = shape.mHasMaterial ? (shape.mVertexColorMode == 0          ? 0u
                                                                        : shape.mVertexColorMode == 1 ? 1u
                                                                                                      : 2u)
                                                              : 2u;
                return shared("vertexcolor:" + std::to_string(mode), [&] {
                    Block* block;
                    const std::int32_t index = mBlocks.add("NiVertexColorProperty", block);
                    Out& out = block->mData;
                    writeObjectNET(out, std::string());
                    out.u16(0); // flags
                    out.u32(mode);
                    out.u32(1); // lighting: emissive, ambient and diffuse
                    return index;
                });
            }

            std::int32_t twoSided()
            {
                return shared("twosided", [&] {
                    Block* block;
                    const std::int32_t index = mBlocks.add("NiStencilProperty", block);
                    Out& out = block->mData;
                    writeObjectNET(out, std::string());
                    out.u16(0); // flags
                    out.u8(0); // stencil off
                    out.u32(0); // test function
                    out.u32(0); // reference
                    out.u32(0xffffffff); // mask
                    out.u32(0); // fail: keep
                    out.u32(0); // z fail: keep
                    out.u32(0); // pass: keep
                    out.u32(3); // draw both sides
                    return index;
                });
            }
        };

        // A shape's triangles in pieces an NiTriShape can hold (65535 vertices and triangles each).
        std::vector<DistantShape> split(const DistantShape& shape)
        {
            constexpr std::size_t maxVertices = std::numeric_limits<std::uint16_t>::max();
            constexpr std::size_t maxTriangles = std::numeric_limits<std::uint16_t>::max();
            if (shape.mPositions.size() <= maxVertices && shape.mIndices.size() / 3 <= maxTriangles)
                return { shape };
            std::vector<DistantShape> result;
            DistantShape piece;
            std::unordered_map<unsigned int, std::uint16_t> remap;
            const auto start = [&] {
                piece = shape;
                piece.mPositions.clear();
                piece.mNormals.clear();
                piece.mColors.clear();
                piece.mTexCoords.clear();
                piece.mIndices.clear();
                remap.clear();
            };
            const auto finish = [&] {
                if (!piece.mIndices.empty())
                    result.push_back(std::move(piece));
            };
            start();
            for (std::size_t i = 0; i + 2 < shape.mIndices.size(); i += 3)
            {
                if (piece.mPositions.size() + 3 > maxVertices || piece.mIndices.size() / 3 + 1 > maxTriangles)
                {
                    finish();
                    start();
                }
                for (std::size_t k = 0; k < 3; ++k)
                {
                    const unsigned int index = shape.mIndices[i + k];
                    auto [it, inserted] = remap.emplace(index, static_cast<std::uint16_t>(piece.mPositions.size()));
                    if (inserted)
                    {
                        piece.mPositions.push_back(shape.mPositions[index]);
                        piece.mNormals.push_back(shape.mNormals[index]);
                        if (!shape.mColors.empty())
                            piece.mColors.push_back(shape.mColors[index]);
                        if (!shape.mTexCoords.empty())
                            piece.mTexCoords.push_back(shape.mTexCoords[index]);
                    }
                    piece.mIndices.push_back(it->second);
                }
            }
            finish();
            return result;
        }

        std::int32_t writeShapeData(Blocks& blocks, const DistantShape& shape)
        {
            Block* block;
            const std::int32_t index = blocks.add("NiTriShapeData", block);
            Out& out = block->mData;
            const std::uint16_t count = static_cast<std::uint16_t>(shape.mPositions.size());
            out.u16(count);
            out.boolean(true);
            for (const osg::Vec3f& v : shape.mPositions)
                out.vec3(v);
            out.boolean(true);
            for (const osg::Vec3f& n : shape.mNormals)
                out.vec3(n);
            osg::BoundingSphere bounds;
            for (const osg::Vec3f& v : shape.mPositions)
                bounds.expandBy(v);
            out.vec3(bounds.center());
            out.f32(bounds.radius());
            out.boolean(!shape.mColors.empty());
            for (const osg::Vec4f& c : shape.mColors)
            {
                out.f32(c.r());
                out.f32(c.g());
                out.f32(c.b());
                out.f32(c.a());
            }
            const bool hasUV = !shape.mTexCoords.empty();
            out.u16(hasUV ? 1 : 0); // number of UV sets
            out.boolean(hasUV);
            for (const osg::Vec2f& uv : shape.mTexCoords)
            {
                out.f32(uv.x());
                out.f32(uv.y());
            }
            const std::uint16_t triangles = static_cast<std::uint16_t>(shape.mIndices.size() / 3);
            out.u16(triangles);
            out.u32(static_cast<std::uint32_t>(triangles) * 3);
            for (std::size_t i = 0; i < static_cast<std::size_t>(triangles) * 3; ++i)
                out.u16(static_cast<std::uint16_t>(shape.mIndices[i]));
            out.u16(0); // match groups
            return index;
        }
    }

    void writeNif(std::ostream& stream, const std::string& rootName, const std::vector<DistantShape>& shapes)
    {
        Blocks blocks;
        Block* root;
        blocks.add("NiNode", root);
        Properties properties(blocks);

        std::vector<std::int32_t> children;
        std::size_t number = 0;
        for (const DistantShape& whole : shapes)
            for (const DistantShape& shape : split(whole))
            {
                if (shape.mIndices.empty())
                    continue;
                const std::vector<std::int32_t> props = properties.get(shape);
                const std::int32_t data = writeShapeData(blocks, shape);
                Block* block;
                const std::int32_t index = blocks.add("NiTriShape", block);
                writeAVObject(block->mData, "Tri Shape " + std::to_string(number++), props);
                block->mData.i32(data);
                block->mData.i32(-1); // skin
                children.push_back(index);
            }

        writeAVObject(root->mData, rootName, {});
        root->mData.u32(static_cast<std::uint32_t>(children.size()));
        for (std::int32_t child : children)
            root->mData.i32(child);
        root->mData.u32(0); // effects

        Out out;
        out.text("NetImmerse File Format, Version 4.0.0.2\n");
        out.u32(0x04000002);
        out.u32(static_cast<std::uint32_t>(blocks.size()));
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            out.string(blocks[i].mType);
            out.text(blocks[i].mData.data());
        }
        out.u32(1); // roots
        out.i32(0);
        stream.write(out.data().data(), static_cast<std::streamsize>(out.data().size()));
    }
}
