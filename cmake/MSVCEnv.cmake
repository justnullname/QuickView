# MSVC Environment Auto-Detection Script for QuickView
# This script extracts INCLUDE paths for the compiler.
# Note: LIB paths are handled by cmake/lld-link-wrapper.bat for maximum reliability.

if(WIN32)
    message(STATUS "[MSVCEnv] Detecting MSVC/Windows SDK environment...")

    # 1. Find vswhere.exe
    set(vswhere_path "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
    if(NOT EXISTS "${vswhere_path}")
        return()
    endif()

    # 2. Find VS installation path
    execute_process(
        COMMAND "${vswhere_path}" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        OUTPUT_VARIABLE vs_install_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(NOT vs_install_path)
        message(STATUS "[MSVCEnv] No compatible Visual Studio detected.")
        return()
    endif()

    # 3. Run vcvarsall.bat
    set(vcvarsall_bat "${vs_install_path}/VC/Auxiliary/Build/vcvarsall.bat")
    execute_process(
        COMMAND cmd /c "\"${vcvarsall_bat}\" x64 && set"
        OUTPUT_VARIABLE vcvars_output
        ERROR_QUIET
    )

    # 4. Extract and apply INCLUDE paths for the compiler
    if(vcvars_output MATCHES "INCLUDE=([^\\r\\n]+)")
        set(val "${CMAKE_MATCH_1}")
        string(REPLACE ";" "\n" val_list "${val}")
        string(REPLACE "\\" "/" val_list "${val_list}")
        string(REPLACE "\n" ";" val_list "${val_list}")
        
        foreach(path ${val_list})
            if(EXISTS "${path}")
                include_directories(SYSTEM "${path}")
            endif()
        endforeach()
        set(ENV{INCLUDE} "${val}")
        message(STATUS "[MSVCEnv] Injected INCLUDE paths from: ${vs_install_path}")
    endif()
    
    # Still set LIB env for standard linker probes that might occur
    if(vcvars_output MATCHES "LIB=([^\\r\\n]+)")
        set(ENV{LIB} "${CMAKE_MATCH_1}")
    endif()
endif()
