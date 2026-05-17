cmake_minimum_required(VERSION 3.20)

project(AstralDB VERSION 0.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASTRALDB_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

add_library(astraldb STATIC
    sources/Database/Database.cxx
    sources/Database/AdvancedTypes.cxx
    sources/Database/MathSci.cxx
    sources/Database/MathSciSignal.cxx
    sources/Database/MathSciAutograd.cxx
    sources/Database/MathSciSolves.cxx
    sources/Database/ColumnarStorage.cxx
    sources/Database/HybridStorageScheduler.cxx
    sources/Database/HybridTable.cxx
    sources/Database/TimeSeries.cxx
    sources/Database/User.cxx
    sources/Database/WriteAheadLog.cxx
    sources/IO/SIMD.cxx
    sources/SQL/AST.cxx
    sources/SQL/Bytecode.cxx
    sources/SQL/BytecodeFormat.cxx
    sources/SQL/BytecodeInspect.cxx
    sources/SQL/BytecodeDebug.cxx
    sources/SQL/BytecodeProcedures.cxx
    sources/SQL/Codegen.cxx
    sources/SQL/Optimizer.cxx
    sources/SQL/Parser.cxx
    sources/SQL/SetExprEval.cxx
    sources/SQL/JsonSql.cxx
    sources/SQL/XmlSql.cxx
    sources/Database/TextIndex.cxx
    sources/Database/VectorIndex.cxx
    sources/SQL/MatchRecognize.cxx
    sources/SQL/TextSearch.cxx
    sources/DS/XML.cxx
)

target_include_directories(astraldb
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/sources
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(MSVC)
    target_compile_options(astraldb PRIVATE /W4 /O2)
    if(ASTRALDB_WARNINGS_AS_ERRORS)
        target_compile_options(astraldb PRIVATE /WX)
    endif()
else()
    target_compile_options(astraldb PRIVATE -Wall -Wextra -O2)
    if(ASTRALDB_WARNINGS_AS_ERRORS)
        target_compile_options(astraldb PRIVATE -Werror)
    endif()
endif()

add_executable(astraldb_cli
    sources/Program.cxx
)
target_link_libraries(astraldb_cli PRIVATE astraldb)

add_executable(astraldb_tests
    tests/TestMain.cxx
    tests/AstralDB.Tests.cxx
)
target_link_libraries(astraldb_tests PRIVATE astraldb)
target_include_directories(astraldb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(astraldb_tests PROPERTIES OUTPUT_NAME run_tests)

enable_testing()
add_test(NAME astraldb_all_tests COMMAND $<TARGET_FILE:astraldb_tests>)
