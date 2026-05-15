cmake_minimum_required(VERSION 3.25)

# ============================================================================
# Project Configuration
# ============================================================================
# Dynamic MSVC environment setup for Clang-CL
include(cmake/MSVCEnv.cmake)

project(QuickView LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force Static CRT for single-binary portability
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Ensure headers from vcpkg-imported targets are treated as SYSTEM to suppress their warnings
set(CMAKE_NO_SYSTEM_FROM_IMPORTED FALSE)

# Automatically handle vcpkg (if not specified via command line)
if(NOT CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
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
    QuickView/ArchiveVFS.cpp
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
    
    # Third party manually included
    third_party/yyjson/yyjson.c
    
    # Resource files
    QuickView/QuickView.rc
)

# ============================================================================
# Executable Target
# ============================================================================
add_subdirectory(third_party/unrar_core)

add_executable(QuickView WIN32 ${QUICKVIEW_SOURCES})

# Explicitly set the subsystem and machine type
set(QUICKVIEW_LINK_FLAGS "/SUBSYSTEM:WINDOWS")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    set(QUICKVIEW_LINK_FLAGS "${QUICKVIEW_LINK_FLAGS} /machine:ARM64")
else()
    set(QUICKVIEW_LINK_FLAGS "${QUICKVIEW_LINK_FLAGS} /machine:x64")
endif()
set_target_properties(QuickView PROPERTIES LINK_FLAGS "${QUICKVIEW_LINK_FLAGS}")

# Include paths
target_include_directories(QuickView PRIVATE QuickView third_party/yyjson third_party/unrar_core)

# Precompiled Headers (PCH) - CXX only to avoid conflict with C source (yyjson.c)
target_precompile_headers(QuickView PRIVATE $<$<COMPILE_LANGUAGE:CXX>:pch.h>)

# Disable PCH for specific files (as defined in .vcxproj)
set_source_files_properties(QuickView/WuffsImpl.cpp PROPERTIES SKIP_PRECOMPILED_HEADERS ON)
set_source_files_properties(QuickView/ImageLoaderSimd.cpp PROPERTIES SKIP_PRECOMPILED_HEADERS ON)
set_source_files_properties(third_party/yyjson/yyjson.c PROPERTIES SKIP_PRECOMPILED_HEADERS ON)

# ============================================================================
# Compiler & Linker Flags (Clang-CL Matrix)
# ============================================================================
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Common strict warnings, encoding, standard conformance, and function-level linking
    target_compile_options(QuickView PRIVATE /W4 /WX /utf-8 /Gw /Gy /permissive-)
    
    # [Optimization] Disable compiler identification string to save bytes
    target_compile_options(QuickView PRIVATE /clang:-fno-ident)

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

        # [ARM64 FIX] Detect architecture for ASan library naming
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
                set(ASAN_ARCH "aarch64")
            else()
                set(ASAN_ARCH "x86_64")
            endif()
        else()
            set(ASAN_ARCH "i386")
        endif()

        target_link_libraries(QuickView PRIVATE 
            clang_rt.asan_dynamic-${ASAN_ARCH}.lib 
            clang_rt.asan_dynamic_runtime_thunk-${ASAN_ARCH}.lib
        )
    endif()

    if(ENABLE_LTO)
        message(STATUS "Build Matrix: Enabling Full LTO (Release-LTO Mode)")
        # Peak performance flags: 
        # /fp:fast (maximum floating point speed) 
        # /GS- (disable security checks for raw performance)
        # /EHs-c- (disable exceptions)
        # -fno-unwind-tables (disable stack unwind tables for binary size)
        target_compile_options(QuickView PRIVATE 
            /O2 /Gw /Zc:inline /GR- /fp:fast /GS- /EHs-c-
            /clang:-flto=full /clang:-mllvm /clang:-inline-threshold=250
            /clang:-fwhole-program-vtables /clang:-fvirtual-function-elimination
            /clang:-fno-unwind-tables
            /clang:-fno-asynchronous-unwind-tables
            /clang:-fno-ident)
        # /MERGE:.rdata=.text (merge read-only data into code section to minimize alignment padding)
        target_link_options(QuickView PRIVATE /OPT:REF /OPT:ICF /opt:lldlto=3 "/MAP:${CMAKE_BINARY_DIR}/QuickView.map" "/MERGE:.rdata=.text")
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

# highway
find_package(hwy CONFIG REQUIRED)

# LibRaw
find_package(libraw CONFIG REQUIRED)

# libjxl (manual find since vcpkg port lacks config)
find_library(JXL_LIB_RELEASE NAMES jxl PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(JXL_LIB_DEBUG NAMES jxl PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
find_library(JXL_THREADS_LIB_RELEASE NAMES jxl_threads PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(JXL_THREADS_LIB_DEBUG NAMES jxl_threads PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
find_library(JXL_CMS_LIB_RELEASE NAMES jxl_cms PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(JXL_CMS_LIB_DEBUG NAMES jxl_cms PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
find_path(JXL_INC jxl/decode.h REQUIRED)

add_library(libjxl::jxl STATIC IMPORTED)
set_target_properties(libjxl::jxl PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${JXL_INC}"
    INTERFACE_COMPILE_DEFINITIONS "JXL_STATIC_DEFINE"
)
set_property(TARGET libjxl::jxl PROPERTY IMPORTED_LOCATION_RELEASE "${JXL_LIB_RELEASE}")
set_property(TARGET libjxl::jxl PROPERTY IMPORTED_LOCATION_DEBUG "${JXL_LIB_DEBUG}")

add_library(libjxl::jxl_threads STATIC IMPORTED)
set_property(TARGET libjxl::jxl_threads PROPERTY IMPORTED_LOCATION_RELEASE "${JXL_THREADS_LIB_RELEASE}")
set_property(TARGET libjxl::jxl_threads PROPERTY IMPORTED_LOCATION_DEBUG "${JXL_THREADS_LIB_DEBUG}")

add_library(libjxl::jxl_cms STATIC IMPORTED)
set_property(TARGET libjxl::jxl_cms PROPERTY IMPORTED_LOCATION_RELEASE "${JXL_CMS_LIB_RELEASE}")
set_property(TARGET libjxl::jxl_cms PROPERTY IMPORTED_LOCATION_DEBUG "${JXL_CMS_LIB_DEBUG}")

# Brotli (required by libjxl)
find_library(BROTLI_COMMON_LIB_RELEASE NAMES brotlicommon PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(BROTLI_COMMON_LIB_DEBUG NAMES brotlicommon PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
find_library(BROTLI_DEC_LIB_RELEASE NAMES brotlidec PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(BROTLI_DEC_LIB_DEBUG NAMES brotlidec PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)

# dav1d (required by avif)
find_library(DAV1D_LIB_RELEASE NAMES dav1d PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(DAV1D_LIB_DEBUG NAMES dav1d PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)

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

# Create targets for Brotli and Dav1d
add_library(brotli::common STATIC IMPORTED)
set_property(TARGET brotli::common PROPERTY IMPORTED_LOCATION_RELEASE "${BROTLI_COMMON_LIB_RELEASE}")
set_property(TARGET brotli::common PROPERTY IMPORTED_LOCATION_DEBUG "${BROTLI_COMMON_LIB_DEBUG}")

add_library(brotli::dec STATIC IMPORTED)
set_property(TARGET brotli::dec PROPERTY IMPORTED_LOCATION_RELEASE "${BROTLI_DEC_LIB_RELEASE}")
set_property(TARGET brotli::dec PROPERTY IMPORTED_LOCATION_DEBUG "${BROTLI_DEC_LIB_DEBUG}")

add_library(dav1d::dav1d STATIC IMPORTED)
set_property(TARGET dav1d::dav1d PROPERTY IMPORTED_LOCATION_RELEASE "${DAV1D_LIB_RELEASE}")
set_property(TARGET dav1d::dav1d PROPERTY IMPORTED_LOCATION_DEBUG "${DAV1D_LIB_DEBUG}")

target_link_libraries(QuickView PRIVATE
    avif
    libjpeg-turbo::turbojpeg-static
    hwy::hwy
    libjxl::jxl
    libjxl::jxl_threads
    libjxl::jxl_cms
    brotli::common
    brotli::dec
    dav1d::dav1d
    lcms2::lcms2
    WebP::webpdemux
    WebP::webpdecoder
    libraw::raw
    yuv
    ZLIB::ZLIB
    unrar_core
    # Windows system libraries
    dwrite.lib
    windowscodecs.lib
    shlwapi.lib
    shell32.lib
    mscms.lib
)
