# Adaptive Toolchain Discovery for QuickView
# This module dynamically locates build tools and MSVC environment paths.

if(ADAPTIVE_TOOLCHAIN_INCLUDED)
    return()
endif()
set(ADAPTIVE_TOOLCHAIN_INCLUDED ON)

message(STATUS "[AdaptiveToolchain] Initializing tool discovery...")

# Bypass cross-compilation search restrictions for host-run build tools
set(_OLD_FIND_ROOT_PATH_MODE_PROGRAM ${CMAKE_FIND_ROOT_PATH_MODE_PROGRAM})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# 1. Locate Visual Studio Installation
set(VSWHERE_SEARCH_PATHS 
    "C:/Program Files (x86)/Microsoft Visual Studio/Installer"
    "C:/Program Files/Microsoft Visual Studio/Installer"
)

find_program(VSWHERE_EXE vswhere 
    PATHS ${VSWHERE_SEARCH_PATHS}
)

set(VS_INSTALL_PATH "")
if(VSWHERE_EXE)
    execute_process(
        COMMAND "${VSWHERE_EXE}" -latest -property installationPath
        OUTPUT_VARIABLE VS_INSTALL_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "[AdaptiveToolchain] Found VS at: ${VS_INSTALL_PATH}")
endif()

# 2. Extract MSVC Environment (Headers/Libs) if on Windows
# This replaces the old MSVCEnv.cmake logic with a cleaner approach.
if(WIN32 AND VS_INSTALL_PATH)
    set(VCVARS_BAT "${VS_INSTALL_PATH}/VC/Auxiliary/Build/vcvarsall.bat")
    if(EXISTS "${VCVARS_BAT}")
        message(STATUS "[AdaptiveToolchain] Extracting MSVC environment via vcvarsall.bat...")
        execute_process(
            COMMAND cmd /c "${VCVARS_BAT}" x64 && set
            OUTPUT_VARIABLE VCVARS_OUTPUT
            ERROR_QUIET
        )
        
        # Helper to extract variable from 'set' output
        macro(extract_vc_var VAR_NAME OUT_VAR)
            string(REGEX MATCH "${VAR_NAME}=([^\\r\\n]*)" _MATCH "${VCVARS_OUTPUT}")
            if(_MATCH)
                set(${OUT_VAR} "${CMAKE_MATCH_1}")
                file(TO_CMAKE_PATH "${${OUT_VAR}}" ${OUT_VAR}_LIST)
                set(ADAPTIVE_MSVC_${VAR_NAME}_PATHS "${${OUT_VAR}_LIST}" CACHE INTERNAL "MSVC ${VAR_NAME} Paths")
            endif()
        endmacro()

        extract_vc_var("INCLUDE" ADAPTIVE_INC)
        extract_vc_var("LIB" ADAPTIVE_LIB)
    endif()
endif()

# 3. Define LLVM Search Hints
set(LLVM_HINTS
    "$ENV{LLVM_INSTALL_DIR}/bin"
    "C:/Program Files/LLVM/bin"
    "C:/Program Files (x86)/LLVM/bin"
    "D:/Program Files/LLVM/bin"
)

if(VS_INSTALL_PATH)
    list(APPEND LLVM_HINTS "${VS_INSTALL_PATH}/VC/Tools/Llvm/x64/bin")
    list(APPEND LLVM_HINTS "${VS_INSTALL_PATH}/VC/Tools/Llvm/bin")
endif()

# 4. Locate LLVM Core Tools
find_program(ADAPTIVE_CLANG_CL clang-cl HINTS ${LLVM_HINTS})
find_program(ADAPTIVE_LLD_LINK lld-link HINTS ${LLVM_HINTS})
find_program(ADAPTIVE_LLVM_LIB  llvm-lib HINTS ${LLVM_HINTS})
find_program(ADAPTIVE_LLVM_RC   llvm-rc   HINTS ${LLVM_HINTS})

# 5. Locate Ninja and NASM (Prioritize vcpkg downloads for project consistency)
get_filename_component(WORKSPACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VCPKG_TOOLS_DIR "${WORKSPACE_ROOT}/third_party/vcpkg/downloads/tools")
if(DEFINED ENV{VCPKG_DOWNLOADS})
    set(VCPKG_TOOLS_DIR "$ENV{VCPKG_DOWNLOADS}/tools")
endif()

# Search for Ninja
set(NINJA_HINTS "PATH")
if(VS_INSTALL_PATH)
    list(APPEND NINJA_HINTS "${VS_INSTALL_PATH}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja")
    list(APPEND NINJA_HINTS "${VS_INSTALL_PATH}/VC/Tools/Llvm/x64/bin")
    list(APPEND NINJA_HINTS "${VS_INSTALL_PATH}/VC/Tools/Llvm/bin")
endif()
list(APPEND NINJA_HINTS "C:/Program Files/CMake/bin")
list(APPEND NINJA_HINTS "C:/Program Files (x86)/CMake/bin")
list(APPEND NINJA_HINTS "C:/Program Files/LLVM/bin")

file(GLOB NINJA_EXES "${VCPKG_TOOLS_DIR}/ninja-*/ninja.exe")
if(NINJA_EXES)
    list(SORT NINJA_EXES ORDER DESCENDING)
    list(GET NINJA_EXES 0 LATEST_NINJA)
    get_filename_component(NINJA_DIR "${LATEST_NINJA}" DIRECTORY)
    list(INSERT NINJA_HINTS 0 "${NINJA_DIR}") # prioritize latest downloaded ninja
endif()

# Search for NASM
set(NASM_HINTS "PATH")
if(VS_INSTALL_PATH)
    list(APPEND NASM_HINTS "${VS_INSTALL_PATH}/VC/Tools/Asm/nasm")
endif()

file(GLOB NASM_EXES "${VCPKG_TOOLS_DIR}/nasm/*/nasm.exe")
if(NASM_EXES)
    list(SORT NASM_EXES ORDER DESCENDING)
    list(GET NASM_EXES 0 LATEST_NASM)
    get_filename_component(NASM_DIR "${LATEST_NASM}" DIRECTORY)
    list(INSERT NASM_HINTS 0 "${NASM_DIR}") # prioritize latest downloaded nasm
endif()

find_program(ADAPTIVE_NINJA ninja HINTS ${NINJA_HINTS})
find_program(ADAPTIVE_NASM  nasm  HINTS ${NASM_HINTS})

# Restore original find program mode
if(_OLD_FIND_ROOT_PATH_MODE_PROGRAM)
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ${_OLD_FIND_ROOT_PATH_MODE_PROGRAM})
else()
    unset(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM)
endif()

# 6. ASan Library Discovery
if(ADAPTIVE_CLANG_CL)
    get_filename_component(LLVM_BIN_DIR "${ADAPTIVE_CLANG_CL}" DIRECTORY)
    get_filename_component(LLVM_ROOT_DIR "${LLVM_BIN_DIR}" DIRECTORY)
    file(GLOB CLANG_LIB_DIRS "${LLVM_ROOT_DIR}/lib/clang/*")
    if(CLANG_LIB_DIRS)
        list(SORT CLANG_LIB_DIRS ORDER DESCENDING)
        list(GET CLANG_LIB_DIRS 0 LATEST_CLANG_LIB)
        set(ADAPTIVE_ASAN_LIB_PATH "${LATEST_CLANG_LIB}/lib/windows")
    endif()
endif()

# 7. Helper to inject paths into environment
macro(adaptive_inject_env)
    set(EXTRA_PATHS "")
    if(ADAPTIVE_CLANG_CL)
        get_filename_component(P "${ADAPTIVE_CLANG_CL}" DIRECTORY)
        list(APPEND EXTRA_PATHS "${P}")
    endif()
    if(ADAPTIVE_NINJA)
        get_filename_component(P "${ADAPTIVE_NINJA}" DIRECTORY)
        list(APPEND EXTRA_PATHS "${P}")
    endif()
    if(ADAPTIVE_NASM)
        get_filename_component(P "${ADAPTIVE_NASM}" DIRECTORY)
        list(APPEND EXTRA_PATHS "${P}")
    endif()
    if(ADAPTIVE_ASAN_LIB_PATH)
        list(APPEND EXTRA_PATHS "${ADAPTIVE_ASAN_LIB_PATH}")
    endif()

    foreach(P IN LISTS EXTRA_PATHS)
        set(ENV{PATH} "${P};$ENV{PATH}")
    endforeach()
    
    # Also inject INCLUDE/LIB for Clang-cl if found
    if(ADAPTIVE_MSVC_INCLUDE_PATHS)
        set(ENV{INCLUDE} "${ADAPTIVE_MSVC_INCLUDE}")
    endif()
    if(ADAPTIVE_MSVC_LIB_PATHS)
        set(ENV{LIB} "${ADAPTIVE_MSVC_LIB}")
    endif()
endmacro()

# 8. Report Findings
macro(report_tool NAME PATH)
    if(${PATH})
        message(STATUS "[AdaptiveToolchain] Found ${NAME}: ${${PATH}}")
    else()
        message(WARNING "[AdaptiveToolchain] Could NOT find ${NAME}")
    endif()
endmacro()

report_tool("Clang-cl" ADAPTIVE_CLANG_CL)
report_tool("lld-link" ADAPTIVE_LLD_LINK)
report_tool("Ninja"    ADAPTIVE_NINJA)
report_tool("NASM"     ADAPTIVE_NASM)
