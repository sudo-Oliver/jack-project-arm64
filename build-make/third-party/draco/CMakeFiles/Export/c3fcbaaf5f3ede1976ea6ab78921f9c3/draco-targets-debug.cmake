#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "draco::draco_static" for configuration "Debug"
set_property(TARGET draco::draco_static APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(draco::draco_static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libdraco.a"
  )

list(APPEND _cmake_import_check_targets draco::draco_static )
list(APPEND _cmake_import_check_files_for_draco::draco_static "${_IMPORT_PREFIX}/lib/libdraco.a" )

# Import target "draco::draco" for configuration "Debug"
set_property(TARGET draco::draco APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(draco::draco PROPERTIES
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libdraco.9.0.0.dylib"
  IMPORTED_SONAME_DEBUG "@rpath/libdraco.9.dylib"
  )

list(APPEND _cmake_import_check_targets draco::draco )
list(APPEND _cmake_import_check_files_for_draco::draco "${_IMPORT_PREFIX}/lib/libdraco.9.0.0.dylib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
