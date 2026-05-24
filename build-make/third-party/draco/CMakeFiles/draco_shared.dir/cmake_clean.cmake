file(REMOVE_RECURSE
  ".9"
  "libdraco.9.0.0.dylib"
  "libdraco.9.dylib"
  "libdraco.dylib"
  "libdraco.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/draco_shared.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
