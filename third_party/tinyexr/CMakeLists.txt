cmake_minimum_required(VERSION 3.20)
project(tinyexr C)

set(TINYEXR_SOURCES
    src/exr_attr.c
    src/exr_b44.c
    src/exr_codec.c
    src/exr_color.c
    src/exr_context.c
    src/exr_convert.c
    src/exr_core.c
    src/exr_cpu.c
    src/exr_deep.c
    src/exr_half.c
    src/exr_jph.c
    src/exr_jph_simd.c
    src/exr_jph_simd_neon.c
    src/exr_piz.c
    src/exr_pxr24.c
    src/exr_reader.c
    src/exr_rle.c
    src/exr_simd_neon.c
    src/exr_simd_x86.c
    src/exr_source.c
    src/exr_source_part.c
    src/exr_stdio.c
    src/exr_thread.c
    src/exr_util_simd_neon.c
    src/exr_util_simd_x86.c
    src/exr_zip.c
    src/exr_zlib_ng_shim.c
    src/exr_zstd.c
    deps/zstd/tinyexr_zstd.c
)

add_library(tinyexr STATIC ${TINYEXR_SOURCES})

set_target_properties(tinyexr PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
set_source_files_properties(${TINYEXR_SOURCES} PROPERTIES SKIP_PRECOMPILED_HEADERS ON)

target_include_directories(tinyexr
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/deps/zstd"
)

target_compile_definitions(tinyexr PRIVATE
    EXR_USE_THREADS
    EXR_ZSTD_DECODE_ONLY
    EXR_JPH_ENABLE_AVX2_FOUR_QUAD16=1
    _CRT_SECURE_NO_WARNINGS
)

if(NOT TARGET ZLIB::ZLIB)
    find_package(ZLIB REQUIRED)
endif()
target_link_libraries(tinyexr PRIVATE ZLIB::ZLIB)

if(MSVC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(tinyexr PRIVATE
        /W0
        $<$<CONFIG:Release>:/O2 /Gw /Gy /Zc:inline>
    )
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(tinyexr PRIVATE
            $<$<CONFIG:Release>:/clang:-fno-ident /clang:-fno-unwind-tables /clang:-fno-asynchronous-unwind-tables /clang:-flto=full>
        )
    endif()
endif()
