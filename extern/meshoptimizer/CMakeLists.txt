# meshoptimizer 0.22 (https://github.com/zeux/meshoptimizer, MIT licence, see LICENSE.md): only the simplifier and
# the vertex cache optimizer.
add_library(meshoptimizer STATIC
    allocator.cpp
    simplifier.cpp
    vcacheoptimizer.cpp
    meshoptimizer.h
)
target_include_directories(meshoptimizer SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)
