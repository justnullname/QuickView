#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "yuv" for configuration "Debug"
set_property(TARGET yuv APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(yuv PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/lib/yuv.lib"
  )

list(APPEND _cmake_import_check_targets yuv )
list(APPEND _cmake_import_check_files_for_yuv "${_IMPORT_PREFIX}/debug/lib/yuv.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
