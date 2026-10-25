#!/usr/bin/env bash
# Adds the tool to an OpenMW source tree (OpenMGE XE fork, moneymitch7/openmw) so that it builds with it:
#   ./add-to-openmw.sh path/to/openmw
# then configure OpenMW as usual and build the target openmw-distantlandtool.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
openmw="${1:?usage: add-to-openmw.sh path/to/openmw}"
rm -rf "${openmw:?}/apps/distantlandtool"
mkdir -p "$openmw/apps/distantlandtool"
cp "$here"/src/* "$openmw/apps/distantlandtool/"
cp -r "$here/extern/meshoptimizer" "$openmw/apps/distantlandtool/meshoptimizer"
if ! grep -q "add_subdirectory(apps/distantlandtool)" "$openmw/CMakeLists.txt"; then
    printf '\n# openmw-distantlandtool (added by add-to-openmw.sh)\nadd_subdirectory(apps/distantlandtool)\n' >> "$openmw/CMakeLists.txt"
fi
echo "Added openmw-distantlandtool to $openmw"
