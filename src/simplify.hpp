#ifndef OPENMW_APPS_DISTANTLANDTOOL_SIMPLIFY_H
#define OPENMW_APPS_DISTANTLANDTOOL_SIMPLIFY_H

#include <osg/Node>
#include <osg/ref_ptr>

#include <cstddef>
#include <string>

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
    };

    struct SimplifyResult
    {
        // empty when the model is left as it is (mSkipReason says why)
        osg::ref_ptr<osg::Node> mNode;
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
}

#endif
