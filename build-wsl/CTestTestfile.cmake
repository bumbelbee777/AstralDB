# CMake generated Testfile for 
# Source directory: /mnt/c/Users/bumbe/Downloads/AstralDB
# Build directory: /mnt/c/Users/bumbe/Downloads/AstralDB/build-wsl
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[astraldb_fast_tests]=] "/mnt/c/Users/bumbe/Downloads/AstralDB/build-wsl/run_tests")
set_tests_properties([=[astraldb_fast_tests]=] PROPERTIES  LABELS "fast" _BACKTRACE_TRIPLES "/mnt/c/Users/bumbe/Downloads/AstralDB/CMakeLists.txt;279;add_test;/mnt/c/Users/bumbe/Downloads/AstralDB/CMakeLists.txt;0;")
add_test([=[astraldb_perf_tests]=] "/mnt/c/Users/bumbe/Downloads/AstralDB/build-wsl/run_tests" "--test-suite=perf")
set_tests_properties([=[astraldb_perf_tests]=] PROPERTIES  LABELS "perf" _BACKTRACE_TRIPLES "/mnt/c/Users/bumbe/Downloads/AstralDB/CMakeLists.txt;281;add_test;/mnt/c/Users/bumbe/Downloads/AstralDB/CMakeLists.txt;0;")
