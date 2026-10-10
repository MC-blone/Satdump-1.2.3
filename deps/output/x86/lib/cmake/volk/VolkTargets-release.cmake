#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Volk::volk" for configuration "Release"
set_property(TARGET Volk::volk APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Volk::volk PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libvolk.so"
  IMPORTED_SONAME_RELEASE "libvolk.so"
  )

list(APPEND _cmake_import_check_targets Volk::volk )
list(APPEND _cmake_import_check_files_for_Volk::volk "${_IMPORT_PREFIX}/lib/libvolk.so" )

# Import target "Volk::volk_static" for configuration "Release"
set_property(TARGET Volk::volk_static APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Volk::volk_static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libvolk.a"
  )

list(APPEND _cmake_import_check_targets Volk::volk_static )
list(APPEND _cmake_import_check_files_for_Volk::volk_static "${_IMPORT_PREFIX}/lib/libvolk.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
