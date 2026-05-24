#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "lzokay::lzokay" for configuration "Debug"
set_property(TARGET lzokay::lzokay APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(lzokay::lzokay PROPERTIES
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/liblzokay.dylib"
  IMPORTED_SONAME_DEBUG "@rpath/liblzokay.dylib"
  )

list(APPEND _cmake_import_check_targets lzokay::lzokay )
list(APPEND _cmake_import_check_files_for_lzokay::lzokay "${_IMPORT_PREFIX}/lib/liblzokay.dylib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
