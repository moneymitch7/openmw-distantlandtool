#include "simplify.hpp"

#include <components/misc/constants.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/util.hpp>

#include <meshoptimizer.h>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/FrontFace>
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/PositionAttitudeTransform>
#include <osg/Sequence>
#include <osg/Switch>
#include <osg/Texture2D>
#include <osg/TriangleIndexFunctor>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace DistantLandTool
{
    namespace
    {
        struct Vertex
        {
            std::array<float, 3> mPosition;
            std::array<float, 3> mNormal;
            std::array<float, 2> mTexCoord;
            std::array<float, 4> mColor;
        };

        static_assert(sizeof(Vertex) == 12 * sizeof(float));

        // What a group of shapes looks like: those alike are merged into one.
        struct Look
        {
            osg::ref_ptr<osg::Image> mImage;
            int mWrapS = osg::Texture::REPEAT;
            int mWrapT = osg::Texture::REPEAT;
            osg::ref_ptr<const SceneUtil::Material> mMaterial;
            osg::ref_ptr<const osg::AlphaFunc> mAlphaFunc;
            osg::ref_ptr<const osg::BlendFunc> mBlendFunc;
            bool mBlend = false;
            bool mTwoSided = false;
            bool mHasColors = false;
            bool mClockwise = false; // front faces wound clockwise (not part of the look: turned around when merged)
        };

        struct Group
        {
            Look mLook;
            std::vector<Vertex> mVertices;
            std::vector<unsigned int> mIndices;
        };

        struct TriangleCollector
        {
            std::vector<unsigned int>* mIndices = nullptr;

            void operator()(unsigned int a, unsigned int b, unsigned int c)
            {
                if (a == b || b == c || a == c)
                    return;
                mIndices->push_back(a);
                mIndices->push_back(b);
                mIndices->push_back(c);
            }
        };

        std::string makeKey(const Look& look)
        {
            std::ostringstream key;
            key << (look.mImage != nullptr ? look.mImage->getFileName() : std::string()) << '|' << look.mWrapS << '|'
                << look.mWrapT << '|' << look.mBlend << look.mTwoSided << look.mHasColors << '|';
            if (look.mMaterial != nullptr)
            {
                const SceneUtil::Material& m = *look.mMaterial;
                for (const osg::Vec4f& v : { m.getDiffuse(), m.getAmbient(), m.getSpecular(), m.getEmission() })
                    key << v.x() << ',' << v.y() << ',' << v.z() << ',' << v.w() << ';';
                key << m.getShininess() << ';' << static_cast<int>(m.getVertexColorMode());
            }
            key << '|';
            if (look.mAlphaFunc != nullptr)
                key << look.mAlphaFunc->getFunction() << ',' << look.mAlphaFunc->getReferenceValue();
            key << '|';
            if (look.mBlend && look.mBlendFunc != nullptr)
                key << look.mBlendFunc->getSource() << ',' << look.mBlendFunc->getDestination();
            return key.str();
        }

        bool isOn(const osg::StateSet& stateset, GLenum mode)
        {
            return (stateset.getMode(mode) & osg::StateAttribute::ON) != 0;
        }

        bool isOff(const osg::StateSet& stateset, GLenum mode)
        {
            const osg::StateAttribute::GLModeValue value = stateset.getMode(mode);
            return value != osg::StateAttribute::INHERIT && (value & osg::StateAttribute::ON) == 0;
        }

        class Collector : public osg::NodeVisitor
        {
        public:
            Collector(float distance, float minPartRadius)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mDistance(distance)
                , mMinPartRadius(minPartRadius)
            {
                mMatrices.emplace_back();
                mStateSets.emplace_back(new osg::StateSet);
            }

            std::string mSkipReason;
            std::map<std::string, Group> mGroups;
            std::size_t mTrianglesIn = 0;
            std::size_t mDrawablesIn = 0;

            void apply(osg::Node& node) override
            {
                if (!enter(node))
                    return;
                traverse(node);
                leave(node);
            }

            void apply(osg::Switch& node) override
            {
                if (!enter(node))
                    return;
                for (unsigned int i = 0; i < node.getNumChildren(); ++i)
                    if (node.getValue(i))
                        node.getChild(i)->accept(*this);
                leave(node);
            }

            void apply(osg::LOD& node) override
            {
                if (!enter(node))
                    return;
                for (unsigned int i = 0; i < node.getNumChildren() && i < node.getNumRanges(); ++i)
                    if (node.getMinRange(i) <= mDistance && mDistance < node.getMaxRange(i))
                        node.getChild(i)->accept(*this);
                leave(node);
            }

            void apply(osg::Sequence& node) override
            {
                if (!enter(node))
                    return;
                if (node.getNumChildren() > 0)
                    node.getChild(node.getValue() >= 0 ? static_cast<unsigned int>(node.getValue()) : 0)->accept(*this);
                leave(node);
            }

            void apply(osg::Transform& node) override
            {
                if (!enter(node))
                    return;
                // only plain transforms; one that depends on the view (a billboard) can't be baked
                if (dynamic_cast<osg::MatrixTransform*>(&node) == nullptr
                    && dynamic_cast<osg::PositionAttitudeTransform*>(&node) == nullptr)
                {
                    skip("a transform that depends on the view (" + std::string(node.className()) + ")");
                    leave(node);
                    return;
                }
                osg::Matrix matrix = mMatrices.back();
                node.computeLocalToWorldMatrix(matrix, this);
                mMatrices.push_back(matrix);
                traverse(node);
                mMatrices.pop_back();
                leave(node);
            }

            void apply(osg::Drawable& drawable) override
            {
                if (!enter(drawable))
                    return;
                if (osg::Geometry* geometry = drawable.asGeometry())
                    addGeometry(*geometry);
                else if (std::string_view(drawable.className()).find("Particle") == std::string_view::npos)
                    skip("a shape that isn't plain geometry (" + std::string(drawable.className()) + ")");
                leave(drawable);
            }

        private:
            float mDistance;
            float mMinPartRadius;
            std::vector<osg::Matrix> mMatrices;
            std::vector<osg::ref_ptr<osg::StateSet>> mStateSets;
            std::vector<bool> mPushed;

            void skip(const std::string& reason)
            {
                if (mSkipReason.empty())
                    mSkipReason = reason;
            }

            /// @return false to leave the node out (hidden)
            bool enter(osg::Node& node)
            {
                if (node.getNodeMask() == 0)
                    return false;
                if (node.getUpdateCallback() != nullptr || node.getCullCallback() != nullptr)
                    skip("an animated or view dependent part (" + std::string(node.className()) + ")");
                if (SceneUtil::hasUserDescription(&node, Constants::NightDayLabel))
                    skip("night and day versions");
                if (SceneUtil::hasUserDescription(&node, Constants::HerbalismLabel))
                    skip("a harvestable plant");
                const osg::StateSet* stateset = node.getStateSet();
                if (stateset != nullptr && stateset->getUpdateCallback() != nullptr)
                    skip("an animated texture or material");
                if (stateset != nullptr)
                {
                    osg::ref_ptr<osg::StateSet> combined
                        = new osg::StateSet(*mStateSets.back(), osg::CopyOp::SHALLOW_COPY);
                    combined->merge(*stateset);
                    mStateSets.push_back(combined);
                }
                mPushed.push_back(stateset != nullptr);
                return true;
            }

            void leave(osg::Node& /*node*/)
            {
                if (mPushed.back())
                    mStateSets.pop_back();
                mPushed.pop_back();
            }

            Look getLook(const osg::StateSet& stateset, bool hasColors) const
            {
                Look look;
                if (const osg::Texture2D* texture = dynamic_cast<const osg::Texture2D*>(
                        stateset.getTextureAttribute(0, osg::StateAttribute::TEXTURE)))
                {
                    const std::string& type = SceneUtil::getTextureType(stateset, *texture, 0);
                    if ((type.empty() || type == "diffuseMap") && texture->getImage() != nullptr
                        && !texture->getImage()->getFileName().empty())
                    {
                        look.mImage = const_cast<osg::Image*>(texture->getImage());
                        look.mWrapS = texture->getWrap(osg::Texture::WRAP_S);
                        look.mWrapT = texture->getWrap(osg::Texture::WRAP_T);
                    }
                }
                look.mMaterial
                    = dynamic_cast<const SceneUtil::Material*>(stateset.getAttribute(osg::StateAttribute::MATERIAL));
                look.mAlphaFunc
                    = dynamic_cast<const osg::AlphaFunc*>(stateset.getAttribute(osg::StateAttribute::ALPHAFUNC));
                if (look.mAlphaFunc != nullptr && look.mAlphaFunc->getFunction() == osg::AlphaFunc::ALWAYS)
                    look.mAlphaFunc = nullptr;
                look.mBlend = isOn(stateset, GL_BLEND);
                look.mBlendFunc
                    = dynamic_cast<const osg::BlendFunc*>(stateset.getAttribute(osg::StateAttribute::BLENDFUNC));
                look.mTwoSided = isOff(stateset, GL_CULL_FACE);
                if (const osg::FrontFace* frontFace
                    = dynamic_cast<const osg::FrontFace*>(stateset.getAttribute(osg::StateAttribute::FRONTFACE)))
                    look.mClockwise = frontFace->getMode() == osg::FrontFace::CLOCKWISE;
                look.mHasColors = hasColors;
                return look;
            }

            void addGeometry(osg::Geometry& geometry)
            {
                const osg::Vec3Array* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                if (positions == nullptr || positions->empty())
                    return;
                const std::size_t count = positions->size();

                const osg::Vec3Array* normals = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray());
                if (normals != nullptr
                    && (normals->getBinding() != osg::Array::BIND_PER_VERTEX || normals->size() < count))
                    normals = nullptr;
                const osg::Array* colors = geometry.getColorArray();
                if (colors != nullptr
                    && (colors->getBinding() != osg::Array::BIND_PER_VERTEX || colors->getNumElements() < count))
                    colors = nullptr;
                const osg::Vec4Array* colorsF = dynamic_cast<const osg::Vec4Array*>(colors);
                const osg::Vec4ubArray* colorsUB = dynamic_cast<const osg::Vec4ubArray*>(colors);
                if (colorsF == nullptr && colorsUB == nullptr)
                    colors = nullptr;
                const osg::Vec2Array* texCoords = dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(0));
                if (texCoords != nullptr && texCoords->size() < count)
                    texCoords = nullptr;

                std::vector<unsigned int> indices;
                osg::TriangleIndexFunctor<TriangleCollector> functor;
                functor.mIndices = &indices;
                geometry.accept(functor);
                if (indices.empty())
                    return;
                for (unsigned int index : indices)
                    if (index >= count)
                        return;

                ++mDrawablesIn;
                mTrianglesIn += indices.size() / 3;

                const osg::Matrix& matrix = mMatrices.back();
                const osg::Matrix normalMatrix = osg::Matrix::inverse(matrix);
                // a mirroring transform turns the triangles around
                const bool mirrored = (osg::Vec3d(matrix(0, 0), matrix(0, 1), matrix(0, 2))
                                          ^ osg::Vec3d(matrix(1, 0), matrix(1, 1), matrix(1, 2)))
                        * osg::Vec3d(matrix(2, 0), matrix(2, 1), matrix(2, 2))
                    < 0;

                // the part's size: left out when too small to be seen from afar
                osg::BoundingBox bounds;
                std::vector<Vertex> vertices(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    const osg::Vec3f position = (*positions)[i] * matrix;
                    bounds.expandBy(position);
                    Vertex& v = vertices[i];
                    v.mPosition = { position.x(), position.y(), position.z() };
                    osg::Vec3f normal(0, 0, 1);
                    if (normals != nullptr)
                    {
                        normal = osg::Matrix::transform3x3(normalMatrix, (*normals)[i]);
                        normal.normalize();
                    }
                    v.mNormal = { normal.x(), normal.y(), normal.z() };
                    v.mTexCoord = { 0.f, 0.f };
                    if (texCoords != nullptr)
                        v.mTexCoord = { (*texCoords)[i].x(), (*texCoords)[i].y() };
                    v.mColor = { 1.f, 1.f, 1.f, 1.f };
                    if (colorsF != nullptr)
                        v.mColor = { (*colorsF)[i].r(), (*colorsF)[i].g(), (*colorsF)[i].b(), (*colorsF)[i].a() };
                    else if (colorsUB != nullptr)
                        v.mColor = { (*colorsUB)[i].r() / 255.f, (*colorsUB)[i].g() / 255.f, (*colorsUB)[i].b() / 255.f,
                            (*colorsUB)[i].a() / 255.f };
                }
                if (bounds.radius() < mMinPartRadius)
                    return;

                // (the geometry's own state set is already in, see enter)
                const Look look = getLook(*mStateSets.back(), colors != nullptr);
                const bool turn = mirrored != look.mClockwise;
                Group& group = mGroups[makeKey(look)];
                group.mLook = look;
                const unsigned int base = static_cast<unsigned int>(group.mVertices.size());
                group.mVertices.insert(group.mVertices.end(), vertices.begin(), vertices.end());
                for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
                {
                    group.mIndices.push_back(base + indices[i]);
                    group.mIndices.push_back(base + indices[turn ? i + 2 : i + 1]);
                    group.mIndices.push_back(base + indices[turn ? i + 1 : i + 2]);
                }
            }
        };

        // Exact duplicates (the same position, normal, texture coordinate and colour) made one, so that the
        // simplifier sees the surface as connected.
        void weld(Group& group)
        {
            std::unordered_map<std::string, unsigned int> unique;
            std::vector<Vertex> vertices;
            std::vector<unsigned int> remap(group.mVertices.size());
            for (std::size_t i = 0; i < group.mVertices.size(); ++i)
            {
                std::string key(reinterpret_cast<const char*>(&group.mVertices[i]), sizeof(Vertex));
                auto [it, inserted] = unique.emplace(std::move(key), static_cast<unsigned int>(vertices.size()));
                if (inserted)
                    vertices.push_back(group.mVertices[i]);
                remap[i] = it->second;
            }
            for (unsigned int& index : group.mIndices)
                index = remap[index];
            group.mVertices = std::move(vertices);
        }

        // Only the vertices still used, in the order they are first used.
        void compact(Group& group)
        {
            std::vector<unsigned int> remap(group.mVertices.size(), ~0u);
            std::vector<Vertex> vertices;
            for (unsigned int& index : group.mIndices)
            {
                if (remap[index] == ~0u)
                {
                    remap[index] = static_cast<unsigned int>(vertices.size());
                    vertices.push_back(group.mVertices[index]);
                }
                index = remap[index];
            }
            group.mVertices = std::move(vertices);
        }

        DistantShape makeShape(const Group& group)
        {
            DistantShape shape;
            const Look& look = group.mLook;
            const std::size_t count = group.mVertices.size();
            shape.mPositions.reserve(count);
            shape.mNormals.reserve(count);
            for (const Vertex& v : group.mVertices)
            {
                shape.mPositions.emplace_back(v.mPosition[0], v.mPosition[1], v.mPosition[2]);
                shape.mNormals.emplace_back(v.mNormal[0], v.mNormal[1], v.mNormal[2]);
                if (look.mImage != nullptr)
                    shape.mTexCoords.emplace_back(v.mTexCoord[0], v.mTexCoord[1]);
                if (look.mHasColors)
                    shape.mColors.emplace_back(v.mColor[0], v.mColor[1], v.mColor[2], v.mColor[3]);
            }
            shape.mIndices = group.mIndices;
            if (look.mImage != nullptr)
            {
                shape.mTexture = look.mImage->getFileName();
                shape.mWrapS = look.mWrapS != osg::Texture::CLAMP_TO_EDGE && look.mWrapS != osg::Texture::CLAMP;
                shape.mWrapT = look.mWrapT != osg::Texture::CLAMP_TO_EDGE && look.mWrapT != osg::Texture::CLAMP;
            }
            if (look.mMaterial != nullptr)
            {
                const SceneUtil::Material& m = *look.mMaterial;
                shape.mHasMaterial = true;
                shape.mDiffuse = m.getDiffuse();
                shape.mAmbient = m.getAmbient();
                shape.mSpecular = m.getSpecular();
                shape.mEmission = m.getEmission();
                shape.mShininess = m.getShininess();
                shape.mVertexColorMode = static_cast<int>(m.getVertexColorMode());
            }
            if (look.mAlphaFunc != nullptr)
            {
                shape.mAlphaTest = true;
                shape.mAlphaFunc = look.mAlphaFunc->getFunction();
                shape.mAlphaRef = look.mAlphaFunc->getReferenceValue();
            }
            if (look.mBlend)
            {
                shape.mBlend = true;
                if (look.mBlendFunc != nullptr)
                {
                    shape.mBlendSource = look.mBlendFunc->getSource();
                    shape.mBlendDestination = look.mBlendFunc->getDestination();
                }
            }
            shape.mTwoSided = look.mTwoSided;
            return shape;
        }
    }

    std::string describe(const DistantShape& shape)
    {
        std::ostringstream out;
        out << shape.mIndices.size() / 3 << " triangles, texture \"" << shape.mTexture << "\" wrap " << shape.mWrapS
            << shape.mWrapT << (shape.mColors.empty() ? "" : ", vertex colours");
        if (shape.mHasMaterial)
            out << ", material diffuse " << shape.mDiffuse.r() << ' ' << shape.mDiffuse.g() << ' ' << shape.mDiffuse.b()
                << " alpha " << shape.mDiffuse.a() << " emission " << shape.mEmission.r() << ' ' << shape.mEmission.g()
                << ' ' << shape.mEmission.b() << " colour mode " << shape.mVertexColorMode;
        if (shape.mAlphaTest)
            out << ", alpha test " << shape.mAlphaFunc << ' ' << shape.mAlphaRef;
        if (shape.mBlend)
            out << ", blend " << shape.mBlendSource << ' ' << shape.mBlendDestination;
        if (shape.mTwoSided)
            out << ", two-sided";
        return out.str();
    }

    SimplifyResult simplifyModel(const osg::Node& model, float maxScale, const SimplifyOptions& options)
    {
        SimplifyResult result;
        const float scale = std::max(maxScale, 0.01f);
        Collector collector(options.mDistance / scale, options.mMinPartRadius / scale);
        const_cast<osg::Node&>(model).accept(collector);
        result.mTrianglesIn = collector.mTrianglesIn;
        result.mDrawablesIn = collector.mDrawablesIn;
        if (!collector.mSkipReason.empty())
        {
            result.mSkipReason = collector.mSkipReason;
            return result;
        }
        for (const auto& [key, group] : collector.mGroups)
            if (group.mLook.mImage != nullptr)
                result.mTextures.push_back(group.mLook.mImage->getFileName());
        if (collector.mGroups.empty())
        {
            result.mSkipReason = "no shapes large enough";
            return result;
        }

        const float maxError = options.mMaxError / scale;
        for (auto& [key, group] : collector.mGroups)
        {
            weld(group);
            std::vector<unsigned int> simplified(group.mIndices.size());
            float error = 0.f;
            // no error allowed: merged only, not simplified
            const std::size_t size = maxError <= 0.f
                ? group.mIndices.size()
                : meshopt_simplify(simplified.data(), group.mIndices.data(), group.mIndices.size(),
                      group.mVertices.front().mPosition.data(), group.mVertices.size(), sizeof(Vertex), 0, maxError,
                      meshopt_SimplifyErrorAbsolute, &error);
            if (size < 3)
                continue;
            if (maxError <= 0.f)
                simplified = group.mIndices;
            simplified.resize(size);
            meshopt_optimizeVertexCache(
                simplified.data(), simplified.data(), simplified.size(), group.mVertices.size());
            group.mIndices = std::move(simplified);
            compact(group);
            result.mTrianglesOut += group.mIndices.size() / 3;
            ++result.mDrawablesOut;
            result.mShapes.push_back(makeShape(group));
        }

        if (result.mShapes.empty())
        {
            result.mSkipReason = "nothing left";
            return result;
        }
        // worth it only when there is less to draw: fewer triangles or fewer shapes
        if (!options.mKeepUnchanged && result.mTrianglesOut * 10 > result.mTrianglesIn * 9
            && result.mDrawablesOut >= result.mDrawablesIn)
        {
            result.mSkipReason = "already simple";
            result.mShapes.clear();
            return result;
        }
        return result;
    }
}
