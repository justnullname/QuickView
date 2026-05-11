cmake_minimum_required(VERSION 3.25)

# ============================================================================
# Project Configuration
# ============================================================================
project(QuickView LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ensure headers from vcpkg-imported targets are treated as SYSTEM to suppress their warnings
set(CMAKE_NO_SYSTEM_FROM_IMPORTED FALSE)

# Automatically handle vcpkg (if not specified via command line)
if(NOT CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/QuickView")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# ============================================================================
# Options & Matrix Build Logic
# ============================================================================
option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(ENABLE_LTO  "Enable Link Time Optimization (ThinLTO)" OFF)

# ============================================================================
# Source Files Definition
# ============================================================================
set(QUICKVIEW_SOURCES
    QuickView/main.cpp
    QuickView/pch.cpp
    QuickView/AppStrings.cpp
    QuickView/GeekGlass.cpp
    QuickView/GeekContextMenu.cpp
    QuickView/GeekIconLibrary.cpp
    QuickView/GeekIconRenderer.cpp
    QuickView/GeekIconData.cpp
    QuickView/exif.cpp
    QuickView/RenderEngine.cpp
    QuickView/ImageLoader.cpp
    QuickView/LosslessTransform.cpp
    QuickView/StbLoader.cpp
    QuickView/TinyExrLoader.cpp
    QuickView/WuffsImpl.cpp
    QuickView/ContextMenu.cpp
    QuickView/GalleryOverlay.cpp
    QuickView/ThumbnailManager.cpp
    QuickView/Toolbar.cpp
    QuickView/SettingsOverlay.cpp
    QuickView/HelpOverlay.cpp
    QuickView/UpdateManager.cpp
    QuickView/PreviewExtractor.cpp
    QuickView/ImageEngine.cpp
    QuickView/CompositionEngine.cpp
    QuickView/UIRenderer.cpp
    QuickView/HeavyLanePool.cpp
    QuickView/TileMemoryManager.cpp
    QuickView/TileManager.cpp
    QuickView/TileScheduler.cpp
    QuickView/ComputeEngine.cpp
    QuickView/DisplayColorInfo.cpp
    QuickView/ProcessRouter.cpp
    QuickView/ThemeSystem.cpp
    QuickView/ImageLoaderSimd.cpp
    QuickView/WebPAnimator.cpp
    QuickView/AvifAnimator.cpp
    QuickView/JxlAnimator.cpp
    QuickView/QuickViewETW.cpp
    
    # Resource files
    QuickView/QuickView.rc
)

# ============================================================================
# Executable Target
# ============================================================================
add_executable(QuickView WIN32 ${QUICKVIEW_SOURCES})

# Explicitly set the subsystem to Windows to prevent console window and ensure correct entry point
set_target_properties(QuickView PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")

# Include paths
target_include_directories(QuickView PRIVATE QuickView)

# Precompiled Headers (PCH)
target_precompile_headers(QuickView PRIVATE QuickView/pch.h)

# Disable PCH for specific files (as defined in .vcxproj)
set_source_files_properties(QuickView/WuffsImpl.cpp PROPERTIES SKIP_PRECOMPILED_HEADERS ON)
set_source_files_properties(QuickView/ImageLoaderSimd.cpp PROPERTIES SKIP_PRECOMPILED_HEADERS ON)

# ============================================================================
# Compiler & Linker Flags (Clang-CL Matrix)
# ============================================================================
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Common strict warnings, encoding, standard conformance, and function-level linking
    target_compile_options(QuickView PRIVATE /W4 /utf-8 /Gw /Gy /permissive-)
    
    # Disable automatic manifest generation (we provide it via .rc)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
    
    if(ENABLE_ASAN)
        message(STATUS "Build Matrix: Enabling ASan (Debug-ASan Mode)")
        set(ASAN_FLAGS "-fsanitize=address" "/Oy-" "-D_DISABLE_STRING_ANNOTATION" "-D_DISABLE_VECTOR_ANNOTATION")
        # Keep precise floating point and default security checks in ASan environment
        target_compile_options(QuickView PRIVATE ${ASAN_FLAGS} /Od /fp:precise)
        
        # [ASAN FIX] Manually link ASan runtime for lld-link
        if(DEFINED ENV{LLVM_INSTALL_DIR} AND DEFINED ENV{LLVM_VERSION})
            set(ASAN_LIB_DIR "$ENV{LLVM_INSTALL_DIR}/lib/clang/$ENV{LLVM_VERSION}/lib/windows")
            target_link_directories(QuickView PRIVATE ${ASAN_LIB_DIR})
        endif()
        target_link_libraries(QuickView PRIVATE 
            clang_rt.asan_dynamic-x86_64.lib 
            clang_rt.asan_dynamic_runtime_thunk-x86_64.lib
        )
    endif()

    if(ENABLE_LTO)
        message(STATUS "Build Matrix: Enabling Full LTO (Release-LTO Mode)")
        # Peak performance flags: /fp:fast (maximum floating point speed) and /GS- (disable security checks for raw performance)
        target_compile_options(QuickView PRIVATE /O2 /Gw /Zc:inline /GR- /fp:fast /GS- -flto -mllvm -inline-threshold=250)
        target_link_options(QuickView PRIVATE -flto /OPT:REF /OPT:ICF /MAP)
    endif()
endif()

# ============================================================================
# Manifest & Entry Point
# ============================================================================
set_target_properties(QuickView PROPERTIES 
    WIN32_EXECUTABLE TRUE
)
# Note: Manifest is handled via QuickView.rc

# ============================================================================
# Dependencies (vcpkg)
# ============================================================================
# libavif
find_package(libavif CONFIG REQUIRED)

# mimalloc
find_package(mimalloc CONFIG REQUIRED)

# highway
find_package(hwy CONFIG REQUIRED)

# LibRaw
find_package(libraw CONFIG REQUIRED)

# libjxl (manual find since vcpkg port lacks config)
find_library(JXL_LIB jxl REQUIRED)
find_library(JXL_THREADS_LIB jxl_threads REQUIRED)
find_library(JXL_CMS_LIB jxl_cms REQUIRED)
find_path(JXL_INC jxl/decode.h REQUIRED)

add_library(libjxl::jxl STATIC IMPORTED)
set_target_properties(libjxl::jxl PROPERTIES
    IMPORTED_LOCATION "${JXL_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${JXL_INC}"
    INTERFACE_COMPILE_DEFINITIONS "JXL_STATIC_DEFINE"
)
add_library(libjxl::jxl_threads STATIC IMPORTED)
set_target_properties(libjxl::jxl_threads PROPERTIES
    IMPORTED_LOCATION "${JXL_THREADS_LIB}"
)
add_library(libjxl::jxl_cms STATIC IMPORTED)
set_target_properties(libjxl::jxl_cms PROPERTIES
    IMPORTED_LOCATION "${JXL_CMS_LIB}"
)

# Brotli (required by libjxl)
find_library(BROTLI_COMMON_LIB brotlicommon REQUIRED)
find_library(BROTLI_DEC_LIB brotlidec REQUIRED)
find_library(BROTLI_ENC_LIB brotlienc REQUIRED)

# dav1d (required by avif)
find_library(DAV1D_LIB dav1d REQUIRED)

# lcms2
find_package(lcms2 CONFIG REQUIRED)

# WebP
find_package(WebP CONFIG REQUIRED)

# libjpeg-turbo
find_package(libjpeg-turbo CONFIG REQUIRED)

# libyuv
find_package(libyuv CONFIG REQUIRED)

# ZLIB
find_package(ZLIB REQUIRED)

target_link_libraries(QuickView PRIVATE
    avif
    mimalloc
    libjpeg-turbo::turbojpeg-static
    libjpeg-turbo::jpeg-static
    hwy::hwy
    libjxl::jxl
    libjxl::jxl_threads
    libjxl::jxl_cms
    "${BROTLI_DEC_LIB}"
    "${BROTLI_COMMON_LIB}"
    "${BROTLI_ENC_LIB}"
    "${DAV1D_LIB}"
    lcms2::lcms2
    WebP::webp
    WebP::webpdemux
    WebP::webpdecoder
    libraw::raw
    yuv
    ZLIB::ZLIB
    # Windows system libraries
    dwrite.lib
    windowscodecs.lib
    shlwapi.lib
    shell32.lib
    mscms.lib
)
