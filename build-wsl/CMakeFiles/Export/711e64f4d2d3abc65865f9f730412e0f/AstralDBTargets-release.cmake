#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "AstralDB::astraldb_core" for configuration "Release"
set_property(TARGET AstralDB::astraldb_core APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(AstralDB::astraldb_core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libastraldb_core.a"
  )

list(APPEND _cmake_import_check_targets AstralDB::astraldb_core )
list(APPEND _cmake_import_check_files_for_AstralDB::astraldb_core "${_IMPORT_PREFIX}/lib/libastraldb_core.a" )

# Import target "AstralDB::astraldb_c" for configuration "Release"
set_property(TARGET AstralDB::astraldb_c APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(AstralDB::astraldb_c PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libastraldb_c.a"
  )

list(APPEND _cmake_import_check_targets AstralDB::astraldb_c )
list(APPEND _cmake_import_check_files_for_AstralDB::astraldb_c "${_IMPORT_PREFIX}/lib/libastraldb_c.a" )

# Import target "AstralDB::astraldb" for configuration "Release"
set_property(TARGET AstralDB::astraldb APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(AstralDB::astraldb PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/astraldb"
  )

list(APPEND _cmake_import_check_targets AstralDB::astraldb )
list(APPEND _cmake_import_check_files_for_AstralDB::astraldb "${_IMPORT_PREFIX}/bin/astraldb" )

# Import target "AstralDB::astraldb_tests" for configuration "Release"
set_property(TARGET AstralDB::astraldb_tests APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(AstralDB::astraldb_tests PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/run_tests"
  )

list(APPEND _cmake_import_check_targets AstralDB::astraldb_tests )
list(APPEND _cmake_import_check_files_for_AstralDB::astraldb_tests "${_IMPORT_PREFIX}/bin/run_tests" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
