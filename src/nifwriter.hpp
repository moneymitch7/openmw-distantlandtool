#ifndef OPENMW_APPS_DISTANTLANDTOOL_NIFWRITER_H
#define OPENMW_APPS_DISTANTLANDTOOL_NIFWRITER_H

#include "simplify.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace DistantLandTool
{
    /// Writes the shapes as a Morrowind NIF (version 4.0.0.2): a root NiNode with an NiTriShape per shape (split where
    /// a shape has more vertices or triangles than one can hold), each with its texture, material, alpha, vertex
    /// colour and two-sided properties. Any OpenMW (and NifSkope) reads it.
    void writeNif(std::ostream& stream, const std::string& rootName, const std::vector<DistantShape>& shapes);
}

#endif
