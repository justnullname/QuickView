cmake_minimum_required(VERSION 3.25)

# ============================================================================
# Adaptive Build Tool Discovery (Handled by Toolchain)
# ============================================================================

# Force-define CXX features to bypass CMake's Clang 22 feature detection bug
project(QuickView LANGUAGES C CXX RC)

if(WIN32 AND ADAPTIVE_MSVC_INCLUDE_PATHS)
    include_directories(SYSTEM ${ADAPTIVE_MSVC_INCLUDE_PATHS})
endif()

set(CMAKE_CXX_COMPILE_FEATURES "cxx_std_11;cxx_std_14;cxx_std_17;cxx_std_20;cxx_std_23;cxx_std_26" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX98_COMPILE_FEATURES "cxx_template_template_parameters" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX11_COMPILE_FEATURES "cxx_alias_templates;cxx_alignas;cxx_alignof;cxx_attributes;cxx_auto_type;cxx_constexpr;cxx_decltype;cxx_defaulted_functions;cxx_defaulted_move_initializers;cxx_delegating_constructors;cxx_deleted_functions;cxx_explicit_conversions;cxx_extended_friend_declarations;cxx_extern_templates;cxx_final;cxx_func_identifier;cxx_generalized_initializers;cxx_inheriting_constructors;cxx_inline_namespaces;cxx_lambdas;cxx_local_type_template_args;cxx_long_long_type;cxx_noexcept;cxx_nonstatic_member_init;cxx_nullptr;cxx_override;cxx_range_for;cxx_raw_string_literals;cxx_reference_qualified_functions;cxx_right_angle_brackets;cxx_rvalue_references;cxx_sizeof_member;cxx_static_assert;cxx_strong_enums;cxx_thread_local;cxx_trailing_return_types;cxx_unicode_literals;cxx_uniform_initialization;cxx_unrestricted_unions;cxx_user_literals;cxx_variadic_macros;cxx_variadic_templates" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX14_COMPILE_FEATURES "cxx_aggregate_default_initializers;cxx_attribute_deprecated;cxx_binary_literals;cxx_contextual_conversions;cxx_decltype_auto;cxx_digit_separators;cxx_generic_lambdas;cxx_lambda_init_captures;cxx_local_type_template_args;cxx_relaxed_constexpr;cxx_return_type_deduction;cxx_variable_templates" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX17_COMPILE_FEATURES "cxx_std_17" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX20_COMPILE_FEATURES "cxx_std_20" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX23_COMPILE_FEATURES "cxx_std_23" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX26_COMPILE_FEATURES "cxx_std_26" CACHE INTERNAL "" FORCE)

# Set normal variables to override the compiler detection's empty normal variables
set(CMAKE_CXX_COMPILE_FEATURES "${CMAKE_CXX_COMPILE_FEATURES}")
set(CMAKE_CXX98_COMPILE_FEATURES "${CMAKE_CXX98_COMPILE_FEATURES}")
set(CMAKE_CXX11_COMPILE_FEATURES "${CMAKE_CXX11_COMPILE_FEATURES}")
set(CMAKE_CXX14_COMPILE_FEATURES "${CMAKE_CXX14_COMPILE_FEATURES}")
set(CMAKE_CXX17_COMPILE_FEATURES "${CMAKE_CXX17_COMPILE_FEATURES}")
set(CMAKE_CXX20_COMPILE_FEATURES "${CMAKE_CXX20_COMPILE_FEATURES}")
set(CMAKE_CXX23_COMPILE_FEATURES "${CMAKE_CXX23_COMPILE_FEATURES}")
set(CMAKE_CXX26_COMPILE_FEATURES "${CMAKE_CXX26_COMPILE_FEATURES}")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
option(ENABLE_SECURITY_COOKIE "Enable /GS buffer security check for Store/WACK compliance" ON)

# ============================================================================
# Source Files Definition
# ============================================================================
set(QUICKVIEW_SOURCES
    QuickView/main.cpp
    QuickView/StringUtils.cpp
    QuickView/ViewPanController.cpp
    QuickView/AsyncJob.cpp
    QuickView/BootPreloader.cpp
    QuickView/ImageExporter.cpp
    QuickView/ExportPanel.cpp
    QuickView/PrintManager.cpp
    QuickView/PrintPreviewUI.cpp
    QuickView/PrintManager.h
    QuickView/FileNavigator.cpp
    QuickView/AppContext.cpp
    QuickView/CompareController.cpp
    QuickView/DialogController.cpp
    QuickView/ZoomAnimation.cpp
    QuickView/ColorMath.cpp
    QuickView/pch.cpp
    QuickView/AppStrings.cpp
    QuickView/GeekGlass.cpp
    QuickView/GeekContextMenu.cpp
    QuickView/GeekIconLibrary.cpp
    QuickView/GeekIconRenderer.cpp
    QuickView/GeekIconData.cpp
    QuickView/exif.cpp
    QuickView/RatingMetadata.cpp
    QuickView/RatingStore.cpp
    QuickView/RatingWriter.cpp
    QuickView/RenderEngine.cpp
    QuickView/ImageLoader.cpp
    QuickView/MiniTiff.cpp
    QuickView/MiniTiffLzw.cpp
    QuickView/MiniTiffCmyk.cpp
    QuickView/IcoDecoder.cpp
    QuickView/PdfDecoder.cpp
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
    QuickView/FileAssociationManager.cpp
    QuickView/HelpOverlay.cpp
    QuickView/UpdateManager.cpp
    QuickView/PreviewExtractor.cpp
    QuickView/ImageEngine.cpp
    QuickView/CompositionEngine.cpp
    QuickView/GeekWidgets.cpp
    QuickView/UIRenderer.cpp
    QuickView/HeavyLanePool.cpp
    QuickView/TileMemoryManager.cpp
    QuickView/TileManager.cpp
    QuickView/TileScheduler.cpp
    QuickView/ComputeEngine.cpp
    QuickView/DisplayColorInfo.cpp
    QuickView/ProcessRouter.cpp
    QuickView/ThemePalette.cpp
    QuickView/ThemeSystem.cpp
    QuickView/ImageLoaderSimd.cpp
    QuickView/WebPAnimator.cpp
    QuickView/AvifAnimator.cpp
    QuickView/JxlAnimator.cpp
    QuickView/QuickViewETW.cpp
    QuickView/OffscreenWebView2.cpp
    QuickView/WebContentHost.cpp
    QuickView/WebViewThumbService.cpp
    QuickView/Plugin/PluginRegistry.cpp
    QuickView/Plugin/PluginHost.cpp
    QuickView/ThumbWorker.cpp
    QuickView/AiActionManager.cpp
    QuickView/AiActionOverlay.cpp
    QuickView/WinOcrBackend.cpp
    QuickView/OcrManager.cpp
    QuickView/OcrOverlay.cpp
    
    # Third party manually included
    third_party/yyjson/yyjson.c
    
    # Resource files
    QuickView/QuickView.rc
)

# ============================================================================
# Executable Target
# ============================================================================
add_subdirectory(third_party/unrar-mini)

add_executable(QuickView WIN32 ${QUICKVIEW_SOURCES})

# Explicitly set the subsystem and machine type
set(QUICKVIEW_LINK_FLAGS "/SUBSYSTEM:WINDOWS")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    set(QUICKVIEW_LINK_FLAGS "${QUICKVIEW_LINK_FLAGS} /machine:ARM64")
    set(QV_WEBVIEW2_ARCH win-arm64)
else()
    set(QUICKVIEW_LINK_FLAGS "${QUICKVIEW_LINK_FLAGS} /machine:x64")
    set(QV_WEBVIEW2_ARCH win-x64)
endif()
set(QV_WEBVIEW2_LIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/${QV_WEBVIEW2_ARCH}/WebView2LoaderStatic.lib")
if(NOT EXISTS "${QV_WEBVIEW2_LIB}")
    message(FATAL_ERROR "WebView2LoaderStatic.lib missing for ${QV_WEBVIEW2_ARCH}: ${QV_WEBVIEW2_LIB}")
endif()
set_target_properties(QuickView PROPERTIES LINK_FLAGS "${QUICKVIEW_LINK_FLAGS}")

# Include paths
target_include_directories(QuickView PRIVATE QuickView third_party/yyjson third_party/unrar-mini third_party/webview2)

# Disable PCH for specific files (as defined in .vcxproj)
set_source_files_properties(QuickView/WuffsImpl.cpp PROPERTIES SKIP_PRECOMPILED_HEADERS ON)
set_source_files_properties(QuickView/ImageLoaderSimd.cpp PROPERTIES SKIP_PRECOMPILED_HEADERS ON)
set_source_files_properties(third_party/yyjson/yyjson.c PROPERTIES SKIP_PRECOMPILED_HEADERS ON)

if(ENABLE_LTO)
    # Pure-cold UI/config/setup translation units: optimize for size (/clang:-Oz).
    # Must skip PCH before target_precompile_headers() so Clang-cl does not reject
    # OptimizationLevel mismatch against the /O2 PCH. UIRenderer.cpp and all hot paths stay at /O2.
    set_target_properties(QuickView PROPERTIES QUICKVIEW_COLD_OZ 1)
    set(QUICKVIEW_COLD_CXX_SOURCES
        QuickView/SettingsOverlay.cpp
        QuickView/HelpOverlay.cpp
        QuickView/UpdateManager.cpp
        QuickView/FileAssociationManager.cpp
        QuickView/ExportPanel.cpp
        QuickView/PrintPreviewUI.cpp
        QuickView/Plugin/PluginRegistry.cpp
        QuickView/Plugin/PluginHost.cpp
        QuickView/ThumbWorker.cpp
        QuickView/WinOcrBackend.cpp
        QuickView/OcrManager.cpp
        QuickView/OcrOverlay.cpp
    )
    set_source_files_properties(${QUICKVIEW_COLD_CXX_SOURCES} PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS "$<$<BOOL:$<TARGET_PROPERTY:QUICKVIEW_COLD_OZ>>:/FIpch.h;/clang:-Oz>"
    )
    set_source_files_properties(third_party/yyjson/yyjson.c PROPERTIES
        COMPILE_OPTIONS "$<$<BOOL:$<TARGET_PROPERTY:QUICKVIEW_COLD_OZ>>:/clang:-Oz>"
    )
endif()

# Precompiled Headers (PCH) - CXX only to avoid conflict with C source (yyjson.c)
target_precompile_headers(QuickView PRIVATE $<$<COMPILE_LANGUAGE:CXX>:pch.h>)

# ============================================================================
# Compiler & Linker Flags (Clang-CL Matrix)
# ============================================================================
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Common strict warnings, encoding, standard conformance, and function-level linking
    target_compile_options(QuickView PRIVATE /W4 /WX /utf-8 /Gw /Gy /permissive-)
    
    # [Optimization] Disable compiler identification string to save bytes
    target_compile_options(QuickView PRIVATE /clang:-fno-ident)

    # [Stability] Lock MSVC compatibility version to prevent PCH mismatch in mixed environments
    if(QUICKVIEW_MSVC_COMPAT_VER)
        add_compile_options("/clang:-fms-compatibility-version=${QUICKVIEW_MSVC_COMPAT_VER}")
    endif()

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
        message(STATUS "Build Matrix: Enabling Full LTO (Release-LTO Pure Maximum Speed)")
        if(ENABLE_SECURITY_COOKIE)
            set(QV_SECURITY_COOKIE_FLAG "/GS")
        else()
            set(QV_SECURITY_COOKIE_FLAG "/GS-")
        endif()
        # Pure peak performance flags for Hot-Path (Pure /O2, no compromise)
        target_compile_options(QuickView PRIVATE 
            /O2 /Gw /Gy /Zc:inline /GR- /fp:fast ${QV_SECURITY_COOKIE_FLAG} /EHs-c- /Z7
            /clang:-flto=full /clang:-fno-exceptions
            /clang:-fwhole-program-vtables /clang:-fvirtual-function-elimination
            /clang:-fno-ident)
        target_link_options(QuickView PRIVATE /DEBUG /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT /opt:lldlto=3 "/MAP:${CMAKE_BINARY_DIR}/QuickView.map")
    endif()
endif()

# ============================================================================
# Manifest & Entry Point
# ============================================================================
set_target_properties(QuickView PROPERTIES 
    WIN32_EXECUTABLE TRUE
)
target_compile_definitions(QuickView PRIVATE UNICODE _UNICODE)
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_link_options(QuickView PRIVATE /STACK:4194304)
endif()

# Note: Manifest is handled via QuickView.rc

# ============================================================================
# Dependencies (vcpkg)
# ============================================================================
# Ensure CMAKE_PREFIX_PATH includes vcpkg target triplet paths
if(VCPKG_TARGET_TRIPLET)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
        list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/installed/${VCPKG_TARGET_TRIPLET}")
        list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/installed/${VCPKG_TARGET_TRIPLET}")
    endif()
endif()

# libjpeg-turbo (Must be before libavif so libyuv's internal find_dependency(JPEG) succeeds)
find_package(libjpeg-turbo CONFIG REQUIRED)
if(NOT TARGET JPEG::JPEG)
    if(TARGET libjpeg-turbo::turbojpeg-static)
        add_library(JPEG::JPEG ALIAS libjpeg-turbo::turbojpeg-static)
    elseif(TARGET libjpeg-turbo::jpeg-static)
        add_library(JPEG::JPEG ALIAS libjpeg-turbo::jpeg-static)
    endif()
endif()

# libavif
find_package(libavif CONFIG REQUIRED)

# highway
find_package(hwy CONFIG REQUIRED)

# LibRaw
find_package(libraw CONFIG REQUIRED)

# Ensure _VCPKG_INSTALLED_DIR fallback
if(NOT _VCPKG_INSTALLED_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed")
        set(_VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/installed")
        set(_VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/installed")
    endif()
endif()

set(_VCPKG_SEARCH_PATHS
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/installed/${VCPKG_TARGET_TRIPLET}"
)

# Reset cache entries if previously trapped in -NOTFOUND
foreach(_VAR JXL_LIB_RELEASE JXL_LIB_DEBUG JXL_THREADS_LIB_RELEASE JXL_THREADS_LIB_DEBUG JXL_CMS_LIB_RELEASE JXL_CMS_LIB_DEBUG BROTLI_COMMON_LIB_RELEASE BROTLI_COMMON_LIB_DEBUG BROTLI_DEC_LIB_RELEASE BROTLI_DEC_LIB_DEBUG DAV1D_LIB_RELEASE DAV1D_LIB_DEBUG)
    if(${_VAR} MATCHES "-NOTFOUND$")
        unset(${_VAR} CACHE)
    endif()
endforeach()

# libjxl (manual find since vcpkg port lacks config)
find_library(JXL_LIB_RELEASE NAMES jxl PATH_SUFFIXES "lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(JXL_LIB_DEBUG NAMES jxl PATH_SUFFIXES "debug/lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(JXL_THREADS_LIB_RELEASE NAMES jxl_threads PATH_SUFFIXES "lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(JXL_THREADS_LIB_DEBUG NAMES jxl_threads PATH_SUFFIXES "debug/lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(JXL_CMS_LIB_RELEASE NAMES jxl_cms PATH_SUFFIXES "lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(JXL_CMS_LIB_DEBUG NAMES jxl_cms PATH_SUFFIXES "debug/lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_path(JXL_INC jxl/decode.h PATH_SUFFIXES "include" PATHS ${_VCPKG_SEARCH_PATHS} REQUIRED)

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
find_library(BROTLI_COMMON_LIB_RELEASE NAMES brotlicommon PATH_SUFFIXES "lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(BROTLI_COMMON_LIB_DEBUG NAMES brotlicommon PATH_SUFFIXES "debug/lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(BROTLI_DEC_LIB_RELEASE NAMES brotlidec PATH_SUFFIXES "lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(BROTLI_DEC_LIB_DEBUG NAMES brotlidec PATH_SUFFIXES "debug/lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(BROTLI_ENC_LIB_RELEASE NAMES brotlienc PATH_SUFFIXES "lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(BROTLI_ENC_LIB_DEBUG NAMES brotlienc PATH_SUFFIXES "debug/lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)

# dav1d (required by avif)
find_library(DAV1D_LIB_RELEASE NAMES dav1d PATH_SUFFIXES "lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)
find_library(DAV1D_LIB_DEBUG NAMES dav1d PATH_SUFFIXES "debug/lib" PATHS ${_VCPKG_SEARCH_PATHS} NO_DEFAULT_PATH)

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

# TinyEXR (pure-C11 v3.4.0 decode core + SIMD + zstd decode-only)
add_subdirectory(third_party/tinyexr)

# Create targets for Brotli and Dav1d
add_library(brotli::common STATIC IMPORTED)
set_property(TARGET brotli::common PROPERTY IMPORTED_LOCATION_RELEASE "${BROTLI_COMMON_LIB_RELEASE}")
set_property(TARGET brotli::common PROPERTY IMPORTED_LOCATION_DEBUG "${BROTLI_COMMON_LIB_DEBUG}")

add_library(brotli::dec STATIC IMPORTED)
set_property(TARGET brotli::dec PROPERTY IMPORTED_LOCATION_RELEASE "${BROTLI_DEC_LIB_RELEASE}")
set_property(TARGET brotli::dec PROPERTY IMPORTED_LOCATION_DEBUG "${BROTLI_DEC_LIB_DEBUG}")

add_library(brotli::enc STATIC IMPORTED)
set_property(TARGET brotli::enc PROPERTY IMPORTED_LOCATION_RELEASE "${BROTLI_ENC_LIB_RELEASE}")
set_property(TARGET brotli::enc PROPERTY IMPORTED_LOCATION_DEBUG "${BROTLI_ENC_LIB_DEBUG}")

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
    tinyexr
    unrar-mini
    # Windows system libraries
    d2d1.lib
    d3d11.lib
    dxgi.lib
    ntdll.lib
    imm32.lib
    dwrite.lib
    windowscodecs.lib
    shcore.lib
    shlwapi.lib
    shell32.lib
    comctl32.lib
    mscms.lib
    gdiplus.lib
    runtimeobject.lib
    ${QV_WEBVIEW2_LIB}
)

# ============================================================================
# Super-Resolution Plugins (QVX)
# ============================================================================
set(NCNN_ARCH "x64")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]64$" OR VCPKG_TARGET_TRIPLET MATCHES "arm64" OR VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64" OR CMAKE_GENERATOR_PLATFORM MATCHES "ARM64" OR CMAKE_CXX_COMPILER_TARGET MATCHES "arm64")
    set(NCNN_ARCH "arm64")
endif()
set(NCNN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ncnn/${NCNN_ARCH}")
if(EXISTS "${NCNN_DIR}/include/ncnn")
    set(_qv_sr_manifest "${CMAKE_CURRENT_SOURCE_DIR}/plugins/sr/sr_ncnn_vulkan/manifest.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_qv_sr_manifest}")
    file(READ "${_qv_sr_manifest}" _qv_sr_manifest_json)
    string(JSON _qv_sr_version ERROR_VARIABLE _qv_sr_ver_err GET "${_qv_sr_manifest_json}" "version")
    if(NOT _qv_sr_version OR _qv_sr_ver_err)
        message(FATAL_ERROR "Failed to read single-source version from ${_qv_sr_manifest}: ${_qv_sr_ver_err}")
    endif()
    add_library(sr_ncnn_vulkan SHARED plugins/sr/sr_ncnn_vulkan/sr_ncnn_vulkan_engine.cpp)
    set_target_properties(sr_ncnn_vulkan PROPERTIES
        SUFFIX ".qvx"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/sr/sr_ncnn_vulkan"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/plugins/sr/sr_ncnn_vulkan"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/plugins/sr/sr_ncnn_vulkan"
    )
    target_compile_definitions(sr_ncnn_vulkan PRIVATE UNICODE _UNICODE QVX_BUILDING_PLUGIN QVX_PLUGIN_VERSION_STR="${_qv_sr_version}")
    if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(sr_ncnn_vulkan PRIVATE /utf-8 /GS)
        target_link_options(sr_ncnn_vulkan PRIVATE /DYNAMICBASE /NXCOMPAT)
    endif()
    target_include_directories(sr_ncnn_vulkan PRIVATE 
        QuickView 
        "${NCNN_DIR}/include"
        "${NCNN_DIR}/include/ncnn"
    )
    target_link_libraries(sr_ncnn_vulkan PRIVATE 
        d3d11.lib dxgi.lib shlwapi.lib
        "${NCNN_DIR}/lib/ncnn.lib"
        "${NCNN_DIR}/lib/glslang.lib"
    )

    # Sync plugin models, README, and manifest to build directory
    add_custom_command(TARGET sr_ncnn_vulkan POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/plugins/sr/sr_ncnn_vulkan/models"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/plugins/sr/sr_ncnn_vulkan/models"
            "${CMAKE_BINARY_DIR}/plugins/sr/sr_ncnn_vulkan/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugins/sr/sr_ncnn_vulkan/README.md"
            "${CMAKE_BINARY_DIR}/plugins/sr/sr_ncnn_vulkan/README.md"
        COMMAND ${CMAKE_COMMAND}
            -DIN=${CMAKE_CURRENT_SOURCE_DIR}/plugins/sr/sr_ncnn_vulkan/manifest.json
            -DOUT=${CMAKE_BINARY_DIR}/plugins/sr/sr_ncnn_vulkan/manifest.json
            -DARCH=${NCNN_ARCH}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StagePluginManifest.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugins/plugins_manifest.json"
            "${CMAKE_BINARY_DIR}/plugins/plugins_manifest.json"
        COMMENT "Deploying isolated sr_ncnn_vulkan plugin assets, README, & manifest..."
    )

    add_dependencies(QuickView sr_ncnn_vulkan)

    # OCR Analyzer Plugin (PP-OCRv5 NCNN Vulkan)
    set(_qv_ocr_manifest "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ocr/ocr_ppocr_ncnn/manifest.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_qv_ocr_manifest}")
    file(READ "${_qv_ocr_manifest}" _qv_ocr_manifest_json)
    string(JSON _qv_ocr_version ERROR_VARIABLE _qv_ocr_ver_err GET "${_qv_ocr_manifest_json}" "version")
    if(NOT _qv_ocr_version OR _qv_ocr_ver_err)
        message(FATAL_ERROR "Failed to read single-source version from ${_qv_ocr_manifest}: ${_qv_ocr_ver_err}")
    endif()
    set(_qv_zxing_src "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zxing-cpp/core/src")
    set_source_files_properties("${_qv_zxing_src}/libzueci/zueci.c" PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        COMPILE_DEFINITIONS "ZUECI_EMBED_NO_TO_ECI"
    )
    add_library(ZXingSlim STATIC
        "${_qv_zxing_src}/Barcode.cpp"
        "${_qv_zxing_src}/BarcodeFormat.cpp"
        "${_qv_zxing_src}/BinaryBitmap.cpp"
        "${_qv_zxing_src}/BitArray.cpp"
        "${_qv_zxing_src}/BitMatrix.cpp"
        "${_qv_zxing_src}/BitSource.cpp"
        "${_qv_zxing_src}/CharacterSet.cpp"
        "${_qv_zxing_src}/ConcentricFinder.cpp"
        "${_qv_zxing_src}/Content.cpp"
        "${_qv_zxing_src}/ECI.cpp"
        "${_qv_zxing_src}/Error.cpp"
        "${_qv_zxing_src}/GenericGF.cpp"
        "${_qv_zxing_src}/GenericGFPoly.cpp"
        "${_qv_zxing_src}/GlobalHistogramBinarizer.cpp"
        "${_qv_zxing_src}/GridSampler.cpp"
        "${_qv_zxing_src}/GTIN.cpp"
        "${_qv_zxing_src}/HRI.cpp"
        "${_qv_zxing_src}/HybridBinarizer.cpp"
        "${_qv_zxing_src}/PerspectiveTransform.cpp"
        "${_qv_zxing_src}/ReadBarcode.cpp"
        "${_qv_zxing_src}/ReedSolomonDecoder.cpp"
        "${_qv_zxing_src}/ResultPoint.cpp"
        "${_qv_zxing_src}/TextDecoder.cpp"
        "${_qv_zxing_src}/Utf.cpp"
        "${_qv_zxing_src}/WhiteRectDetector.cpp"
        "${_qv_zxing_src}/libzueci/zueci.c"
        "${_qv_zxing_src}/qrcode/QRBitMatrixParser.cpp"
        "${_qv_zxing_src}/qrcode/QRCodecMode.cpp"
        "${_qv_zxing_src}/qrcode/QRDataBlock.cpp"
        "${_qv_zxing_src}/qrcode/QRDecoder.cpp"
        "${_qv_zxing_src}/qrcode/QRDetector.cpp"
        "${_qv_zxing_src}/qrcode/QRErrorCorrectionLevel.cpp"
        "${_qv_zxing_src}/qrcode/QRFormatInformation.cpp"
        "${_qv_zxing_src}/qrcode/QRReader.cpp"
        "${_qv_zxing_src}/qrcode/QRVersion.cpp"
        "${_qv_zxing_src}/datamatrix/DMBitLayout.cpp"
        "${_qv_zxing_src}/datamatrix/DMDataBlock.cpp"
        "${_qv_zxing_src}/datamatrix/DMDecoder.cpp"
        "${_qv_zxing_src}/datamatrix/DMDetector.cpp"
        "${_qv_zxing_src}/datamatrix/DMReader.cpp"
        "${_qv_zxing_src}/datamatrix/DMVersion.cpp"
        "${_qv_zxing_src}/oned/ODUPCEANCommon.cpp"
        "${_qv_zxing_src}/oned/ODMultiUPCEANReader.cpp"
        "${_qv_zxing_src}/oned/ODCode128Patterns.cpp"
        "${_qv_zxing_src}/oned/ODCode128Reader.cpp"
        "${_qv_zxing_src}/oned/ODCode39Reader.cpp"
        plugins/ocr/ocr_ppocr_ncnn/zxing_slim_readers.cpp
    )
    set_target_properties(ZXingSlim PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(ZXingSlim PUBLIC "${_qv_zxing_src}")
    target_compile_definitions(ZXingSlim PRIVATE ZXING_READERS UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX)
    if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(ZXingSlim PRIVATE /utf-8 /Gw /Gy)
    endif()
    target_compile_features(ZXingSlim PUBLIC cxx_std_17)

    add_library(ocr_ppocr_ncnn SHARED
        plugins/ocr/ocr_ppocr_ncnn/ocr_ppocr_ncnn_engine.cpp
    )
    set_target_properties(ocr_ppocr_ncnn PROPERTIES
        SUFFIX ".qvx"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/ocr/ocr_ppocr_ncnn"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/plugins/ocr/ocr_ppocr_ncnn"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/plugins/ocr/ocr_ppocr_ncnn"
    )
    target_compile_definitions(ocr_ppocr_ncnn PRIVATE UNICODE _UNICODE QVX_BUILDING_PLUGIN QVX_PLUGIN_VERSION_STR="${_qv_ocr_version}")
    if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(ocr_ppocr_ncnn PRIVATE /utf-8 /Gw /Gy /GS)
        target_link_options(ocr_ppocr_ncnn PRIVATE
            /DYNAMICBASE /NXCOMPAT
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/OPT:REF;/OPT:ICF>
        )
    endif()
    target_include_directories(ocr_ppocr_ncnn PRIVATE
        QuickView
        plugins/ocr/ocr_ppocr_ncnn
        "${NCNN_DIR}/include"
        "${NCNN_DIR}/include/ncnn"
    )
    target_link_libraries(ocr_ppocr_ncnn PRIVATE
        shlwapi.lib
        ZXingSlim
        "${NCNN_DIR}/lib/ncnn.lib"
        "${NCNN_DIR}/lib/glslang.lib"
    )
    add_custom_command(TARGET ocr_ppocr_ncnn POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/plugins/ocr/ocr_ppocr_ncnn/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ocr/ocr_ppocr_ncnn/README.md"
            "${CMAKE_BINARY_DIR}/plugins/ocr/ocr_ppocr_ncnn/README.md"
        COMMAND ${CMAKE_COMMAND}
            -DIN=${CMAKE_CURRENT_SOURCE_DIR}/plugins/ocr/ocr_ppocr_ncnn/manifest.json
            -DOUT=${CMAKE_BINARY_DIR}/plugins/ocr/ocr_ppocr_ncnn/manifest.json
            -DARCH=${NCNN_ARCH}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StagePluginManifest.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugins/plugins_manifest.json"
            "${CMAKE_BINARY_DIR}/plugins/plugins_manifest.json"
        COMMENT "Deploying isolated ocr_ppocr_ncnn plugin assets, README, & manifest..."
    )
    add_dependencies(QuickView ocr_ppocr_ncnn)
endif()

# ============================================================================
# Shell Extension Plugins (QVX + COM IThumbnailProvider)
# ============================================================================
set(_qv_shell_manifest "${CMAKE_CURRENT_SOURCE_DIR}/plugins/shell/qv_shell_thumb/manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_qv_shell_manifest}")
file(READ "${_qv_shell_manifest}" _qv_shell_manifest_json)
string(JSON _qv_shell_version ERROR_VARIABLE _qv_shell_ver_err GET "${_qv_shell_manifest_json}" "version")
if(NOT _qv_shell_version OR _qv_shell_ver_err)
    message(FATAL_ERROR "Failed to read single-source version from ${_qv_shell_manifest}: ${_qv_shell_ver_err}")
endif()
add_library(qv_shell_thumb SHARED
    plugins/shell/qv_shell_thumb/qv_shell_thumb.cpp
    plugins/shell/qv_shell_thumb/qv_shell_thumb.def
)
set_target_properties(qv_shell_thumb PROPERTIES
    SUFFIX ".qvx"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/shell/qv_shell_thumb"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/plugins/shell/qv_shell_thumb"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/plugins/shell/qv_shell_thumb"
)
target_compile_definitions(qv_shell_thumb PRIVATE UNICODE _UNICODE QVX_BUILDING_PLUGIN QVX_PLUGIN_VERSION_STR="${_qv_shell_version}")
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(qv_shell_thumb PRIVATE
        /W4 /WX /utf-8 /Gw /Gy /GR- /EHs-c- /GS
        /clang:-fno-exceptions /clang:-fno-ident
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/clang:-Oz>
    )
    target_link_options(qv_shell_thumb PRIVATE
        /DYNAMICBASE /NXCOMPAT
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/OPT:REF;/OPT:ICF>
    )
endif()
target_include_directories(qv_shell_thumb PRIVATE QuickView)
target_link_libraries(qv_shell_thumb PRIVATE shell32.lib shlwapi.lib ole32.lib propsys.lib advapi32.lib gdi32.lib)
add_custom_command(TARGET qv_shell_thumb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugins/shell/qv_shell_thumb/README.md"
        "${CMAKE_BINARY_DIR}/plugins/shell/qv_shell_thumb/README.md"
    COMMAND ${CMAKE_COMMAND}
        -DIN=${CMAKE_CURRENT_SOURCE_DIR}/plugins/shell/qv_shell_thumb/manifest.json
        -DOUT=${CMAKE_BINARY_DIR}/plugins/shell/qv_shell_thumb/manifest.json
        -DARCH=${NCNN_ARCH}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StagePluginManifest.cmake"
    COMMENT "Deploying qv_shell_thumb README & manifest..."
)
add_dependencies(QuickView qv_shell_thumb)

# ============================================================================
# Mock OCR Analyzer Plugin (for QuickViewTests E2E Verification)
# ============================================================================
add_library(qv_mock_ocr_test SHARED tests/mock_ocr_plugin/mock_ocr_plugin.cpp)
set_target_properties(qv_mock_ocr_test PROPERTIES
    SUFFIX ".qvx"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
)
target_compile_definitions(qv_mock_ocr_test PRIVATE UNICODE _UNICODE QVX_BUILDING_PLUGIN)
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(qv_mock_ocr_test PRIVATE
        /W4 /WX /utf-8 /Gw /Gy /GR- /EHs-c- /GS
        /clang:-fno-exceptions /clang:-fno-ident
    )
endif()
target_include_directories(qv_mock_ocr_test PRIVATE QuickView)

# ============================================================================
# Unit Tests
# ============================================================================
find_package(GTest CONFIG REQUIRED)
enable_testing()

add_executable(QuickViewTests
    tests/ColorMathTests.cpp
    tests/FileNavigatorTests.cpp
    tests/HotkeyTests.cpp
    tests/MultiPageTests.cpp
    tests/PdfDecoderTests.cpp
    tests/StringUtilsTests.cpp
    tests/SupportedExtensionsTests.cpp
    tests/MiniTiffTests.cpp
    tests/Transform2DTests.cpp
    tests/SettingsSliderMathTests.cpp
    tests/SvgNeedsFallbackTests.cpp
    tests/MetafileCodecTests.cpp
    tests/GeekWidgetsTests.cpp
    tests/PluginHostTests.cpp
    tests/ShellThumbTests.cpp
    tests/RatingMetadataTests.cpp
    tests/TileJpegTests.cpp
    tests/AiActionManagerTests.cpp
    tests/FileAssociationTests.cpp
    tests/ThemeSystemTests.cpp
    tests/UpdateManagerTests.cpp
    tests/HdrPipelineTests.cpp
    tests/ColorPipelineTests.cpp
    tests/ViewPanTests.cpp
    tests/OcrTests.cpp
    QuickView/ViewPanController.cpp
    QuickView/TileManager.cpp
    QuickView/AiActionManager.cpp
    QuickView/WinOcrBackend.cpp
    QuickView/OcrManager.cpp
    QuickView/OcrOverlay.cpp
    QuickView/GeekWidgets.cpp
    QuickView/ThemePalette.cpp
    QuickView/GeekIconLibrary.cpp
    QuickView/GeekIconData.cpp
    QuickView/GeekIconRenderer.cpp
    QuickView/OffscreenWebView2.cpp
    QuickView/LosslessTransform.cpp
    QuickView/ImageLoaderSimd.cpp
    QuickView/WuffsImpl.cpp
    QuickView/ComputeEngine.cpp
    QuickView/DisplayColorInfo.cpp
    QuickView/MiniTiff.cpp
    QuickView/MiniTiffLzw.cpp
    QuickView/MiniTiffCmyk.cpp
    QuickView/IcoDecoder.cpp
    QuickView/PdfDecoder.cpp
    QuickView/ColorMath.cpp 
    QuickView/FileNavigator.cpp 
    QuickView/ArchiveVFS.cpp 
    QuickView/exif.cpp
    QuickView/RatingMetadata.cpp
    QuickView/RatingWriter.cpp
    QuickView/RatingStore.cpp
    QuickView/QuickViewETW.cpp
    QuickView/AppStrings.cpp
    QuickView/FileAssociationManager.cpp
    QuickView/UpdateManager.cpp
    QuickView/Plugin/PluginRegistry.cpp
    QuickView/Plugin/PluginHost.cpp
    QuickView/ThumbWorker.cpp
    QuickView/AsyncJob.cpp
    QuickView/StringUtils.cpp
    third_party/yyjson/yyjson.c
    QuickView/pch.cpp
)
if(TARGET sr_ncnn_vulkan)
    add_dependencies(QuickViewTests sr_ncnn_vulkan)
endif()
if(TARGET ocr_ppocr_ncnn)
    add_dependencies(QuickViewTests ocr_ppocr_ncnn)
endif()
if(TARGET qv_shell_thumb)
    add_dependencies(QuickViewTests qv_shell_thumb)
endif()
if(TARGET qv_mock_ocr_test)
    add_dependencies(QuickViewTests qv_mock_ocr_test)
endif()
target_precompile_headers(QuickViewTests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:pch.h>)
target_link_libraries(QuickViewTests PRIVATE 
    GTest::gtest_main 
    GTest::gtest 
    d2d1.lib 
    dwrite.lib 
    windowscodecs.lib 
    dxgi.lib 
    d3d11.lib 
    shcore.lib
    shlwapi.lib 
    shell32.lib 
    comctl32.lib
    mscms.lib
    gdiplus.lib
    winhttp.lib
    crypt32.lib
    runtimeobject.lib
    unrar-mini
    ZLIB::ZLIB
    libjpeg-turbo::turbojpeg-static
    brotli::common
    brotli::dec
    brotli::enc
    libjxl::jxl
    libjxl::jxl_cms
    lcms2::lcms2
    hwy::hwy
)
target_include_directories(QuickViewTests PRIVATE QuickView plugins/ocr/ocr_ppocr_ncnn third_party/yyjson third_party/unrar-mini)
target_compile_definitions(QuickViewTests PRIVATE UNICODE _UNICODE)
add_test(NAME QuickViewTests COMMAND QuickViewTests)

add_executable(QuickViewAnimationTests
    tests/AnimationDecoderTests.cpp
    QuickView/WuffsImpl.cpp
    QuickView/WebPAnimator.cpp
    QuickView/AvifAnimator.cpp
    QuickView/JxlAnimator.cpp
    QuickView/ImageLoaderSimd.cpp
    QuickView/pch.cpp
)
target_precompile_headers(QuickViewAnimationTests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:pch.h>)
target_link_libraries(QuickViewAnimationTests PRIVATE
    GTest::gtest_main
    GTest::gtest
    ZLIB::ZLIB
    brotli::common
    brotli::dec
    brotli::enc
    libjxl::jxl
    libjxl::jxl_threads
    libjxl::jxl_cms
    lcms2::lcms2
    avif
    dav1d::dav1d
    yuv
    WebP::webpdemux
    WebP::libwebpmux
    WebP::webp
    WebP::sharpyuv
    hwy::hwy
)
target_include_directories(QuickViewAnimationTests PRIVATE QuickView)
target_compile_definitions(QuickViewAnimationTests PRIVATE UNICODE _UNICODE)
add_test(NAME QuickViewAnimationTests COMMAND QuickViewAnimationTests)

# ============================================================================
# IDE IntelliSense & Build Environment Helper (Inject Adaptive SDK Include Paths)
# ============================================================================
if(WIN32 AND ADAPTIVE_MSVC_INCLUDE_PATHS)
    include_directories(SYSTEM ${ADAPTIVE_MSVC_INCLUDE_PATHS})
    target_include_directories(QuickView SYSTEM PRIVATE ${ADAPTIVE_MSVC_INCLUDE_PATHS})
    if(TARGET QuickViewTests)
        target_include_directories(QuickViewTests SYSTEM PRIVATE ${ADAPTIVE_MSVC_INCLUDE_PATHS})
    endif()
endif()

# Force-define CXX features to bypass CMake's Clang 22 feature detection bug
