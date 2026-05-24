#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "replxx::replxx" for configuration "Debug"
set_property(TARGET replxx::replxx APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(replxx::replxx PROPERTIES
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libreplxx-d.0.0.4.dylib"
  IMPORTED_SONAME_DEBUG "@rpath/libreplxx-d.0.0.4.dylib"
  )

list(APPEND _cmake_import_check_targets replxx::replxx )
list(APPEND _cmake_import_check_files_for_replxx::replxx "${_IMPORT_PREFIX}/lib/libreplxx-d.0.0.4.dylib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
