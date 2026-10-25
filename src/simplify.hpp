#ifndef OPENMW_APPS_DISTANTLANDTOOL_SIMPLIFY_H
#define OPENMW_APPS_DISTANTLANDTOOL_SIMPLIFY_H

#include <osg/Node>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <cstddef>
#include <string>
#include <vector>

namespace DistantLandTool
{
    struct SimplifyOptions
    {
        // How far the simplified shape may stray from the model, in game units (before the reference's scale). A
        // pixel is about 6 units across a cell's half width away.
        float mMaxError = 4.f;
        // Parts (shapes) smaller than this radius, in game units at the largest scale the model is placed with, are
        // left out: they are less than a pixel or two across from where distant chunks begin.
        float mMinPartRadius = 8.f;
        // The distance the model's LOD nodes are resolved at (the child shown from this far is kept).
        float mDistance = 8192.f;
        // keep the result even when it is no lighter than the model (testing)
        bool mKeepUnchanged = false;
    };

    /// One shape of a distant model, in model space: triangles with what they look like.
    struct DistantShape
    {
        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mNormals;
        std::vector<osg::Vec4f> mColors; // empty: no vertex colours
        std::vector<osg::Vec2f> mTexCoords; // empty: no texture
        std::vector<unsigned int> mIndices;

        std::string mTexture; // the base texture, a path in the data folders (textures/...)
        bool mWrapS = true;
        bool mWrapT = true;

        bool mHasMaterial = false;
        osg::Vec4f mDiffuse{ 1, 1, 1, 1 };
        osg::Vec4f mAmbient{ 1, 1, 1, 1 };
        osg::Vec4f mSpecular{ 0, 0, 0, 1 };
        osg::Vec4f mEmission{ 0, 0, 0, 1 };
        float mShininess = 0.f;
        int mVertexColorMode = 0; // SceneUtil::VertexColorModes

        bool mAlphaTest = false;
        int mAlphaFunc = 0; // osg::AlphaFunc::ComparisonFunction
        float mAlphaRef = 0.f;

        bool mBlend = false;
        int mBlendSource = 0x0302; // GL_SRC_ALPHA
        int mBlendDestination = 0x0303; // GL_ONE_MINUS_SRC_ALPHA

        bool mTwoSided = false;
    };

    struct SimplifyResult
    {
        // empty when the model is left as it is (mSkipReason says why)
        std::vector<DistantShape> mShapes;
        std::string mSkipReason;
        std::size_t mTrianglesIn = 0;
        std::size_t mTrianglesOut = 0;
        std::size_t mDrawablesIn = 0;
        std::size_t mDrawablesOut = 0;
    };

    /// A distant version of a model: its static shapes in model space, those that look alike (texture, material,
    /// alpha, blending, sides) merged into one, simplified within mMaxError, with only the base texture.
    /// @param maxScale the largest scale the model is placed with in the world
    SimplifyResult simplifyModel(const osg::Node& model, float maxScale, const SimplifyOptions& options);

    /// What a shape looks like, in a line (for the log).
    std::string describe(const DistantShape& shape);
}

#endif
