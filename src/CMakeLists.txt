# openmw-distantlandtool, built inside an OpenMW source tree (see the repository's README and CI): copied there as
# apps/distantlandtool, with add_subdirectory(apps/distantlandtool) appended to OpenMW's CMakeLists.txt. Links
# OpenMW's components library and meshoptimizer (extern/meshoptimizer of this repository, copied alongside).
if (NOT TARGET meshoptimizer)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer ${CMAKE_CURRENT_BINARY_DIR}/meshoptimizer)
endif()

add_library(openmw-distantlandtool-lib STATIC simplify.cpp simplify.hpp nifwriter.cpp nifwriter.hpp texturebake.cpp texturebake.hpp)

openmw_add_executable(openmw-distantlandtool main.cpp)

# Bake the distant texture set by default (--texture-size 128) only for builds that read it, the OpenMGE XE fork's
# (distant land stage 2). Other builds ignore the file, so without it the tool skips it unless asked.
option(DISTANTLANDTOOL_TEXTURE_SET "Bake the distant texture set by default" ON)
if (NOT DISTANTLANDTOOL_TEXTURE_SET)
    target_compile_definitions(openmw-distantlandtool PRIVATE DISTANTLANDTOOL_DEFAULT_TEXTURE_SIZE=0)
endif()

target_link_libraries(openmw-distantlandtool openmw-distantlandtool-lib)

target_link_libraries(openmw-distantlandtool-lib
    Boost::program_options
    components
    meshoptimizer
)

if (WIN32)
    target_sources(openmw-distantlandtool PRIVATE ${CMAKE_SOURCE_DIR}/files/windows/other-apps.manifest)
endif()

if (MSVC AND PRECOMPILE_HEADERS_WITH_MSVC)
    target_precompile_headers(openmw-distantlandtool REUSE_FROM components)
endif()
