cmake_minimum_required(VERSION 3.20)

project(AstralDB VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASTRALDB_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AstralDBCompileOptions.cmake)

add_library(astraldb_core STATIC
    sources/Database/Database.cxx
    sources/Database/AdvancedTypes.cxx
    sources/Database/MathSci.cxx
    sources/Database/MathSciSignal.cxx
    sources/Database/MathSciAutograd.cxx
    sources/Database/MathSciSolves.cxx
    sources/Database/MathSciClassify.cxx
    sources/Database/MathSciNlp.cxx
    sources/Database/MathSciEmbeddings.cxx
    sources/Database/MathSciModel.cxx
    sources/Database/MathSciInference.cxx
    sources/Database/MathSciFokkerPlanck.cxx
    sources/Database/MathSciComplex.cxx
    sources/Database/ColumnarStorage.cxx
    sources/Database/HybridStorageScheduler.cxx
    sources/Database/HybridTable.cxx
    sources/Database/TimeSeries.cxx
    sources/Database/TimeSeriesCompression.cxx
    sources/Database/GeoSpatial.cxx
    sources/Database/Graph.cxx
    sources/Database/GraphStorage.cxx
    sources/Database/EmbeddingStorage.cxx
    sources/Database/User.cxx
    sources/Database/WriteAheadLog.cxx
    sources/DS/glTF.cxx
    sources/DS/Geometry2D.cxx
    sources/DS/Regex.cxx
    sources/DS/JSONCodec.cxx
    sources/IO/SIMD.cxx
    sources/IO/MemoryGuard.cxx
    sources/SQL/AST.cxx
    sources/SQL/Bytecode.cxx
    sources/SQL/BytecodeFormat.cxx
    sources/SQL/BytecodeInspect.cxx
    sources/SQL/BytecodeDebug.cxx
    sources/SQL/BytecodeProcedures.cxx
    sources/SQL/BytecodeTriggers.cxx
    sources/Database/TriggerRuntime.cxx
    sources/SQL/Codegen.cxx
    sources/SQL/Optimizer.cxx
    sources/SQL/MathSciOptimizer.cxx
    sources/SQL/GraphOptimizer.cxx
    sources/SQL/GraphCypher.cxx
    sources/SQL/Parser.cxx
    sources/SQL/DialectCompat.cxx
    sources/SQL/ProcedureParser.cxx
    sources/SQL/SetExprEval.cxx
    sources/SQL/JsonSql.cxx
    sources/SQL/XmlSql.cxx
    sources/Database/TextIndex.cxx
    sources/Database/VectorIndex.cxx
    sources/SQL/MatchRecognize.cxx
    sources/SQL/TextSearch.cxx
)

target_include_directories(astraldb_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sources>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(astraldb_core PRIVATE /W4)
    if(ASTRALDB_WARNINGS_AS_ERRORS)
        target_compile_options(astraldb_core PRIVATE /WX)
    endif()
else()
    target_compile_options(astraldb_core PRIVATE -Wall -Wextra)
    if(ASTRALDB_WARNINGS_AS_ERRORS)
        target_compile_options(astraldb_core PRIVATE -Werror)
    endif()
endif()

astraldb_apply_target_options(astraldb_core)

add_library(astraldb_c STATIC
    sources/API/AstralDbC.cxx
)
target_link_libraries(astraldb_c PUBLIC astraldb_core)
target_include_directories(astraldb_c
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
astraldb_apply_target_options(astraldb_c)

add_executable(astraldb
    sources/Program.cxx
)
target_link_libraries(astraldb PRIVATE astraldb_core)
target_link_libraries(astraldb PRIVATE astraldb_c)
astraldb_apply_target_options(astraldb)

add_executable(astraldb_tests
    tests/TestMain.cxx
    tests/AstralDB.Tests.cxx
)
target_link_libraries(astraldb_tests PRIVATE astraldb_core astraldb_c)
target_include_directories(astraldb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(astraldb_tests PROPERTIES OUTPUT_NAME run_tests)
astraldb_apply_target_options(astraldb_tests)

enable_testing()
add_test(NAME astraldb_fast_tests COMMAND $<TARGET_FILE:astraldb_tests>)
set_tests_properties(astraldb_fast_tests PROPERTIES LABELS "fast")
add_test(NAME astraldb_perf_tests COMMAND $<TARGET_FILE:astraldb_tests> --test-suite=perf)
set_tests_properties(astraldb_perf_tests PROPERTIES LABELS "perf")

include(CMakePackageConfigHelpers)

install(TARGETS astraldb_core astraldb_c astraldb astraldb_tests
    EXPORT AstralDBTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES include/astraldb/AstralDB.h DESTINATION include/astraldb)

install(EXPORT AstralDBTargets
    FILE AstralDBTargets.cmake
    NAMESPACE AstralDB::
    DESTINATION lib/cmake/AstralDB
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AstralDBConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/AstralDBConfig.cmake"
"include(\"${CMAKE_CURRENT_LIST_DIR}/AstralDBTargets.cmake\")\n")

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AstralDBConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AstralDBConfigVersion.cmake"
    DESTINATION lib/cmake/AstralDB
)
