set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# Ensure LLVM and Ninja are in PATH for vcpkg and its sub-processes (like Meson)
set(LLVM_BIN "C:/Program Files/LLVM/bin")
# Ninja is currently in a Winget path, we should ideally find it dynamically but let's use the known path for now
# or better, find it and add its directory.
find_program(NINJA_PATH ninja)
if(NINJA_PATH)
    get_filename_component(NINJA_DIR "${NINJA_PATH}" DIRECTORY)
    set(ENV{PATH} "${LLVM_BIN};${NINJA_DIR};$ENV{PATH}")
else()
    set(ENV{PATH} "${LLVM_BIN};$ENV{PATH}")
endif()

# Dynamically find LLVM tools in PATH to get absolute paths required by vcpkg
# If not in PATH, try standard installation paths
set(LLVM_HINTS "${LLVM_BIN}" "C:/Program Files (x86)/LLVM/bin")

find_program(LLVM_C_COMPILER clang-cl HINTS ${LLVM_HINTS})
find_program(LLVM_CXX_COMPILER clang-cl HINTS ${LLVM_HINTS})
find_program(LLVM_LINKER lld-link HINTS ${LLVM_HINTS})
find_program(LLVM_AR llvm-lib HINTS ${LLVM_HINTS})
find_program(LLVM_RC llvm-rc HINTS ${LLVM_HINTS})
find_program(LLVM_MT llvm-mt HINTS ${LLVM_HINTS})

if(NOT LLVM_C_COMPILER OR NOT LLVM_LINKER)
    message(FATAL_ERROR "Could not find LLVM tools (clang-cl, lld-link). Please ensure LLVM is installed and in PATH, or at ${LLVM_HINTS}")
endif()

# Toolchain paths
set(VCPKG_C_COMPILER "${LLVM_C_COMPILER}")
set(VCPKG_CXX_COMPILER "${LLVM_CXX_COMPILER}")
set(VCPKG_LINKER "${LLVM_LINKER}")
set(VCPKG_AR "${LLVM_AR}")

# Common flags (Clang-cl style)
set(COMMON_FLAGS "/DWIN32 /D_WINDOWS /W3 /utf-8 /Gw")
set(VCPKG_C_FLAGS "${COMMON_FLAGS}")
set(VCPKG_CXX_FLAGS "${COMMON_FLAGS}")

# [SIMD FIX] -march=native is disabled here because libjxl/highway 
# fails to dispatch correctly when forced at the global level in CI-like environments.
# Individual ports should handle their SIMD targeting.

# Release optimization: Max speed + ThinLTO
# 1. Compiler flags need /clang:-fuse-ld=lld for driver-led linking (Meson)
# 2. Linker flags MUST be pure lld-link flags
# 3. /GR- (Disable RTTI), /EHs-c- (Disable Exceptions), -fno-ident (Reduce bloat)
set(COMPILER_LTO_FLAGS "-flto /clang:-fuse-ld=lld /clang:-fno-ident")
set(VCPKG_C_FLAGS_RELEASE "/O2 /Oi /Ob2 ${COMPILER_LTO_FLAGS} /DNDEBUG -DHWY_BASELINE_TARGETS=0x800 -DHWY_DISABLED_TARGETS=0x7080")
set(VCPKG_CXX_FLAGS_RELEASE "/O2 /Oi /Ob2 ${COMPILER_LTO_FLAGS} /DNDEBUG -DHWY_BASELINE_TARGETS=0x800 -DHWY_DISABLED_TARGETS=0x7080")

# LibRaw requires exceptions and RTTI. Disable for others to save size.
if(NOT PORT STREQUAL "libraw")
    string(APPEND VCPKG_C_FLAGS_RELEASE " /GR- /EHs-c-")
    string(APPEND VCPKG_CXX_FLAGS_RELEASE " /GR- /EHs-c-")
endif()

# Pure Linker flags for lld-link
set(VCPKG_LINKER_FLAGS_RELEASE "/OPT:REF /OPT:ICF /opt:lldltojobs=all")

# Force internal CMake calls within vcpkg to use Clang-cl toolchain and LOCK identification
set(VCPKG_CMAKE_CONFIGURE_OPTIONS 
    "-DCMAKE_C_COMPILER=${LLVM_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${LLVM_CXX_COMPILER}"
    "-DCMAKE_LINKER=${LLVM_LINKER}"
    "-DCMAKE_AR=${LLVM_AR}"
    "-DCMAKE_RC_COMPILER=${LLVM_RC}"
    "-DCMAKE_MT=${LLVM_MT}"
    "-DCMAKE_C_COMPILER_FRONTEND_VARIANT=MSVC"
    "-DCMAKE_CXX_COMPILER_FRONTEND_VARIANT=MSVC"
    "-DJPEGXL_ENABLE_ENC=OFF"
    "-DJPEGXL_ENABLE_BENCHMARK=OFF"
    "-DJPEGXL_ENABLE_EXAMPLES=OFF"
    "-DLIBRAW_NO_JASPER=ON"
    "-DWEBP_BUILD_ENCODER=OFF"
    "-DWEBP_BUILD_CWEBP=OFF"
    "-DWEBP_BUILD_GIF2WEBP=OFF"
    "-DWEBP_BUILD_IMG2WEBP=OFF"
    "-DWEBP_BUILD_EXTRAS=OFF"
    "-DBROTLI_DISABLE_ENCODER=ON"
    "-DZLIB_GZFILEOP=OFF"
    "-DZLIB_ENABLE_TESTS=OFF"
    "-DHWY_BASELINE_TARGETS=0x800"
    "-DHWY_DISABLED_TARGETS=0x7080"
)

# Per-port optimization overrides
if(PORT STREQUAL "zlib-ng" OR PORT STREQUAL "highway")
    string(APPEND VCPKG_C_FLAGS_RELEASE " /fp:fast")
    string(APPEND VCPKG_CXX_FLAGS_RELEASE " /fp:fast")
endif()

set(ZLIB_COMPAT ON)
