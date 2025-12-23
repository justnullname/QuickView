get_filename_component(VCPKG_IMPORT_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)
set(WebP_VERSION 1.6.0)
set(WEBP_VERSION ${WebP_VERSION})


####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was WebPConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

if(TRUE)
  include(CMakeFindDependencyMacro)
  find_dependency(Threads REQUIRED)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/WebPTargets.cmake")

set_and_check(WebP_INCLUDE_DIR "${PACKAGE_PREFIX_DIR}/include")
set(WebP_INCLUDE_DIRS ${WebP_INCLUDE_DIR})
set(WEBP_INCLUDE_DIRS ${WebP_INCLUDE_DIR})
set(WebP_LIBRARIES "")
include(SelectLibraryConfigurations)
foreach(_vcpkg_libwebp_lib IN ITEMS libwebpdecoder;libwebp;libwebpdemux;libwebpmux sharpyuv)
    find_library(_vcpkg_${_vcpkg_libwebp_lib}_LIBRARY_RELEASE NAMES ${_vcpkg_libwebp_lib} lib${_vcpkg_libwebp_lib} NAMES_PER_DIR PATHS "${VCPKG_IMPORT_PREFIX}/lib" NO_DEFAULT_PATH)
    find_library(_vcpkg_${_vcpkg_libwebp_lib}_LIBRARY_DEBUG NAMES ${_vcpkg_libwebp_lib}d lib${_vcpkg_libwebp_lib}d ${_vcpkg_libwebp_lib} lib${_vcpkg_libwebp_lib} NAMES_PER_DIR PATHS "${VCPKG_IMPORT_PREFIX}/debug/lib" NO_DEFAULT_PATH)
    select_library_configurations(_vcpkg_${_vcpkg_libwebp_lib})
    list(APPEND WebP_LIBRARIES ${_vcpkg_${_vcpkg_libwebp_lib}_LIBRARIES})
endforeach()
set(WEBP_LIBRARIES "${WebP_LIBRARIES}")

check_required_components(WebP)
