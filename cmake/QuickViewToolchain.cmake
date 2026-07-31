# QuickView Toolchain Wrapper
# This file ensures that the MSVC/Windows SDK environment is detected 
# before any compiler tests or vcpkg operations occur.

# 1. Detect environment and tools
include("${CMAKE_CURRENT_LIST_DIR}/AdaptiveToolchain.cmake")

# 2. Ensure VCPKG_INSTALLED_DIR is set to existing installed packages directory
if(NOT VCPKG_INSTALLED_DIR)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../vcpkg_installed")
        set(VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_LIST_DIR}/../vcpkg_installed" CACHE PATH "")
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/vcpkg/installed")
        set(VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/vcpkg/installed" CACHE PATH "")
    endif()
endif()

# 3. Chain-load vcpkg toolchain
set(VCPKG_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../third_party/vcpkg/scripts/buildsystems/vcpkg.cmake")
if(EXISTS "${VCPKG_TOOLCHAIN_FILE}")
    include("${VCPKG_TOOLCHAIN_FILE}")
endif()
