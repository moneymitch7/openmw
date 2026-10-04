if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    set(MOC_SOURCES
        MaskedOcclusionCulling.cpp
        MaskedOcclusionCullingAVX2.cpp
        MaskedOcclusionCullingAVX512.cpp
    )
else()
    # ARM/other: base file only (SSE2/SSE4.1 via sse2neon on ARM)
    set(MOC_SOURCES
        MaskedOcclusionCulling.cpp
    )
endif()

add_library(maskedoc STATIC ${MOC_SOURCES})
target_include_directories(maskedoc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    if(MSVC)
        # Main file must be compiled at SSE2 baseline (no AVX)
        # x64 MSVC: SSE2 is the baseline (/arch:SSE2 only exists for 32-bit), so the
        # dispatcher file needs no flag.
        if (CMAKE_SIZEOF_VOID_P EQUAL 4)
            set_source_files_properties(MaskedOcclusionCulling.cpp PROPERTIES COMPILE_FLAGS "/arch:SSE2")
        endif()
        set_source_files_properties(MaskedOcclusionCullingAVX2.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX2")
        set_source_files_properties(MaskedOcclusionCullingAVX512.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX512")
    else()
        set_source_files_properties(MaskedOcclusionCulling.cpp PROPERTIES COMPILE_FLAGS "-msse4.1 -mxsave")
        set_source_files_properties(MaskedOcclusionCullingAVX2.cpp PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
        set_source_files_properties(MaskedOcclusionCullingAVX512.cpp PROPERTIES COMPILE_FLAGS "-mavx512f -mavx512bw")
    endif()
endif()

# The library picks its SSE4.1 / AVX2 / AVX-512 implementation at runtime with
# CPUID. Keep link-time optimisation away from it, so wide-ISA code from the
# AVX2/AVX-512 files can't be inlined into paths that run on every CPU.
set_target_properties(maskedoc PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF POSITION_INDEPENDENT_CODE ON)
