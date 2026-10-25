# OpenMGE XE: simpler distant versions of the models placed outside, for object paging's distant chunks.
set(DISTANTLANDTOOL_LIB
    simplify.cpp
)

source_group(apps\\distantlandtool FILES ${DISTANTLANDTOOL_LIB} main.cpp)

add_library(openmw-distantlandtool-lib STATIC ${DISTANTLANDTOOL_LIB})

openmw_add_executable(openmw-distantlandtool main.cpp)

target_link_libraries(openmw-distantlandtool openmw-distantlandtool-lib)

target_link_libraries(openmw-distantlandtool-lib
    Boost::program_options
    components
    meshoptimizer
)

if (WIN32)
    target_sources(openmw-distantlandtool PRIVATE ${CMAKE_SOURCE_DIR}/files/windows/other-apps.manifest)
endif()

if (NOT APPLE)
    install(TARGETS openmw-distantlandtool ${runtime_dependency_arguments})
endif()

if (MSVC AND PRECOMPILE_HEADERS_WITH_MSVC)
    target_precompile_headers(openmw-distantlandtool REUSE_FROM components)
endif()
