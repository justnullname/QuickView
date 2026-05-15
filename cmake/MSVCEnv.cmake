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
    if(NOT EXISTS "${vcvarsall_bat}")
        message(STATUS "[MSVCEnv] vcvarsall.bat not found at ${vcvarsall_bat}")
        return()
    endif()

    file(TO_NATIVE_PATH "${vcvarsall_bat}" vcvarsall_bat_native)

    execute_process(
        COMMAND cmd /c ${vcvarsall_bat_native} x64 && set
        OUTPUT_VARIABLE vcvars_output
        RESULT_VARIABLE vcvars_result
        ERROR_VARIABLE vcvars_error
    )
    
    # 4. Extract paths from the output using direct string slicing (regex + semicolons = pain)
    set(vcvars_output_str "${vcvars_output}")
    
    # Extract INCLUDE
    string(FIND "${vcvars_output_str}" "INCLUDE=" pos)
    if(NOT pos EQUAL -1)
        math(EXPR start "${pos} + 8")
        string(SUBSTRING "${vcvars_output_str}" ${start} -1 tail)
        string(FIND "${tail}" "\r\n" eol)
        if(eol EQUAL -1)
            string(FIND "${tail}" "\n" eol)
        endif()
        string(SUBSTRING "${tail}" 0 ${eol} val)
        file(TO_CMAKE_PATH "${val}" val_list)
        set(QUICKVIEW_MSVC_INC_PATHS "${val_list}" CACHE INTERNAL "MSVC Include Paths")
        
        foreach(path ${val_list})
            if(EXISTS "${path}")
                include_directories(SYSTEM "${path}")
            endif()
        endforeach()
        set(ENV{INCLUDE} "${val}")
    endif()

    # Extract LIB
    string(FIND "${vcvars_output_str}" "LIB=" pos)
    if(NOT pos EQUAL -1)
        math(EXPR start "${pos} + 4")
        string(SUBSTRING "${vcvars_output_str}" ${start} -1 tail)
        string(FIND "${tail}" "\r\n" eol)
        if(eol EQUAL -1)
            string(FIND "${tail}" "\n" eol)
        endif()
        string(SUBSTRING "${tail}" 0 ${eol} val)
        file(TO_CMAKE_PATH "${val}" val_list)
        set(QUICKVIEW_MSVC_LIB_PATHS "${val_list}" CACHE INTERNAL "MSVC Lib Paths")
        set(ENV{LIB} "${val}")
    endif()

    if(QUICKVIEW_MSVC_INC_PATHS)
        message(STATUS "[MSVCEnv] Injected MSVC paths")
    else()
        message(WARNING "[MSVCEnv] Failed to extract MSVC paths from vcvarsall output")
    endif()
endif()
