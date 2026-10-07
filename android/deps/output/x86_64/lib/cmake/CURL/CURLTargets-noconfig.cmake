#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "CURL::libcurl_static" for configuration ""
set_property(TARGET CURL::libcurl_static APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(CURL::libcurl_static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "C"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libcurl.a"
  )

list(APPEND _cmake_import_check_targets CURL::libcurl_static )
list(APPEND _cmake_import_check_files_for_CURL::libcurl_static "${_IMPORT_PREFIX}/lib/libcurl.a" )

# Import target "CURL::libcurl_shared" for configuration ""
set_property(TARGET CURL::libcurl_shared APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(CURL::libcurl_shared PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libcurl.so"
  IMPORTED_SONAME_NOCONFIG "libcurl.so"
  )

list(APPEND _cmake_import_check_targets CURL::libcurl_shared )
list(APPEND _cmake_import_check_files_for_CURL::libcurl_shared "${_IMPORT_PREFIX}/lib/libcurl.so" )

# Import target "CURL::curl" for configuration ""
set_property(TARGET CURL::curl APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(CURL::curl PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/bin/curl"
  )

list(APPEND _cmake_import_check_targets CURL::curl )
list(APPEND _cmake_import_check_files_for_CURL::curl "${_IMPORT_PREFIX}/bin/curl" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
