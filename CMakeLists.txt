cmake_minimum_required(VERSION 3.20)

project(AstralDB VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option(ASTRALDB_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(ASTRALDB_IO_URING "Enable Linux io_uring for column shard reads (requires liburing)" OFF)

include(CheckIPOSupported)
check_ipo_supported(RESULT _astraldb_ipo_supported OUTPUT _astraldb_ipo_error)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AstralDBCompileOptions.cmake)
astraldb_configure_release_dist()

if(ASTRALDB_LTO AND _astraldb_ipo_supported)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

add_library(astraldb_core STATIC
    sources/Database/Database.cxx
    sources/Database/DatabaseWalReplay.cxx
    sources/Database/PtBridge/PtBridge.cxx
    sources/Database/Dialect/DualTable.cxx
    sources/Database/Catalog/Triggers.cxx
    sources/Database/Profile/RegionTimer.cxx
    sources/Database/Text/PatternMatch.cxx
    sources/Database/Index/TextSearch.cxx
    sources/Database/Types/AdvancedTypes.cxx
    sources/Database/Types/JsonCell.cxx
    sources/Database/Types/XmlCell.cxx
    sources/Database/MathSci/MathSci.cxx
    sources/Database/MathSci/MathSciGeoParse.cxx
    sources/Database/MathSci/MathSciSignal.cxx
    sources/Database/MathSci/MathSciAutograd.cxx
    sources/Database/MathSci/MathSciAutogradGraph.cxx
    sources/Database/MathSci/MathSciAutogradMicrokernels.cxx
    sources/Database/MathSci/MathSciPrimitiveMicrokernels.cxx
    sources/Database/MathSci/MathSciSolves.cxx
    sources/Database/MathSci/MathSciClassify.cxx
    sources/Database/MathSci/MathSciNlp.cxx
    sources/Database/MathSci/MathSciEmbeddings.cxx
    sources/Database/MathSci/MathSciModel.cxx
    sources/Database/MathSci/MathSciMl.cxx
    sources/Database/MathSci/MathSciInference.cxx
    sources/Database/MathSci/MathSciFokkerPlanck.cxx
    sources/Database/MathSci/MathSciHeatKernel.cxx
    sources/Database/MathSci/MathSciVision.cxx
    sources/Database/MathSci/MathSciListCache.cxx
    sources/Database/MathSci/MathSciComplex.cxx
    sources/Database/Storage/BulkSynthetic.cxx
    sources/Database/Storage/BulkSyntheticScalar.cxx
    sources/Database/Storage/BulkSyntheticPrecompute.cxx
    sources/Database/Storage/BulkSyntheticDerive.cxx
    sources/Database/Storage/PassBitWalk.cxx
    sources/Database/Storage/BulkSyntheticSemistructured.cxx
    sources/Database/Storage/BulkSyntheticPathEval.cxx
    sources/Database/Storage/BulkSyntheticWindowEval.cxx
    sources/Database/Storage/SemistructuredMicrokernels.cxx
    sources/Database/Storage/SemistructuredKernels.cxx
    sources/Database/Storage/SemistructuredLut.cxx
    sources/Database/Storage/SemistructuredResultPolicy.cxx
    sources/Database/Storage/SemistructuredResultStrips.cxx
    sources/Database/Storage/BulkSyntheticPrecomputeColumns.cxx
    sources/Database/Storage/BulkShapePlan.cxx
    sources/Database/Storage/BulkQueryMetadata.cxx
    sources/Database/Storage/ShapeWorkloadRegistry.cxx
    sources/Database/Storage/PredicateKind.cxx
    sources/Database/Storage/GeneralizedLazyGroupBy.cxx
    sources/Database/Storage/LazyStarJoinFilter.cxx
    sources/Database/Storage/LazyStarJoinSelect.cxx
    sources/Database/Storage/ColumnZoneMap.cxx
    sources/Database/Storage/ColumnFilterSimd.cxx
    sources/Database/Storage/VectorizedScan.cxx
    sources/Database/Storage/VectorizedOps.cxx
    sources/Database/Storage/JoinBloomFilter.cxx
    sources/Database/Storage/ColumnarScanFilter.cxx
    sources/Database/Storage/ParallelHashJoin.cxx
    sources/Database/Storage/TableIndexAccess.cxx
    sources/Database/Storage/HierarchyPath.cxx
    sources/Database/Storage/ColumnarStorage.cxx
    sources/Database/Storage/ColumnarLazyBulk.cxx
    sources/Database/Storage/StarJoinCubeBulk.cxx
    sources/Database/Storage/StarJoinCubeAnalytic.cxx
    sources/Database/Storage/StarJoinCubeFusedTail.cxx
    sources/Database/Storage/Superfetch.cxx
    sources/Database/Storage/SimdTiling.cxx
    sources/Database/Storage/Microkernels.cxx
    sources/Database/Storage/OlapAggregateMicrokernels.cxx
    sources/Database/Storage/CompressedColumnStore.cxx
    sources/Database/Storage/ColumnView.cxx
    sources/Database/Storage/ChunkCompressor.cxx
    sources/Database/Storage/LateMaterializer.cxx
    sources/Database/Storage/LsmBypassReader.cxx
    sources/Database/Storage/AggEngine.cxx
    sources/Database/Storage/SparseCube.cxx
    sources/DS/SimdHash.cxx
    sources/DS/FormatDoubleSimd.cxx
    sources/DS/SimdJsonExtract.cxx
    sources/DS/SimdXmlExtract.cxx
    sources/DS/CuckooMap.cxx
    sources/DS/RadixPartition.cxx
    sources/DS/BloomTrie.cxx
    sources/DS/BitSliceIndex.cxx
    sources/DS/Xlsx.cxx
    sources/Database/Storage/HybridStorageScheduler.cxx
    sources/Database/Storage/HybridTable.cxx
    sources/Database/Storage/TimeSeries.cxx
    sources/Database/Storage/TimeSeriesCompression.cxx
    sources/Database/Graph/GeoSpatial.cxx
    sources/Database/Graph/Graph.cxx
    sources/Database/Graph/GraphStorage.cxx
    sources/Database/Graph/GraphLazyBulk.cxx
    sources/Database/Embedding/EmbeddingStorage.cxx
    sources/Database/Security/User.cxx
    sources/Database/Storage/WriteAheadLog.cxx
    sources/DS/glTF.cxx
    sources/DS/Geometry2D.cxx
    sources/DS/Regex.cxx
    sources/DS/JSONCodec.cxx
    sources/IO/SIMD.cxx
    sources/IO/Job.cxx
    sources/IO/MemoryGuard.cxx
    sources/IO/ColumnShardConfig.cxx
    sources/IO/ColumnShardRegistry.cxx
    sources/IO/ColumnShardStream.cxx
    sources/IO/RuntimeLimits.cxx
    sources/DS/ShardCompress.cxx
    sources/SQL/Parser/AST.cxx
    sources/SQL/Bytecode/FastPathGuard.cxx
    sources/SQL/Bytecode/Bytecode.cxx
    sources/SQL/Bytecode/BytecodeBulkOps.cxx
    sources/SQL/Bytecode/OpcodeMeta.cxx
    sources/SQL/Bytecode/BytecodeDisasm.cxx
    sources/SQL/Bytecode/BytecodeFormat.cxx
    sources/SQL/Bytecode/BytecodeInspect.cxx
    sources/SQL/Bytecode/BytecodeDebug.cxx
    sources/SQL/Procedures/BytecodeProcedures.cxx
    sources/SQL/Procedures/BytecodeTriggers.cxx
    sources/SQL/Bytecode/VmRuntime.cxx
    sources/SQL/Parser/Codegen.cxx
    sources/SQL/Optimizer/Optimizer.cxx
    sources/SQL/Optimizer/AdaptiveOptimizer.cxx
    sources/SQL/Optimizer/ConfidenceScoring.cxx
    sources/SQL/Optimizer/RuntimeChecks.cxx
    sources/SQL/Fusion/FusionRules.cxx
    sources/SQL/Fusion/SemistructuredFusionPass.cxx
    sources/SQL/SemistructuredPlanner.cxx
    sources/SQL/SemistructuredVM.cxx
    sources/SQL/Fusion/FusedOps.cxx
    sources/SQL/Bulk/BulkOps.cxx
    sources/SQL/Bulk/BulkDominantAmb.cxx
    sources/SQL/Bulk/BulkDominantWarehouseMegafusion.cxx
    sources/SQL/Bytecode/QueryCheckpoint.cxx
    sources/SQL/Bulk/SqlBytecodeTail.cxx
    sources/SQL/Bulk/BulkOpcodePass.cxx
    sources/SQL/NewVM/VliwScheduler.cxx
    sources/SQL/NewVM/VliwRegisterAlloc.cxx
    sources/SQL/NewVM/VliwExecutor.cxx
    sources/SQL/JIT/JitCompiler.cxx
    sources/SQL/JIT/JitRuntime.cxx
    sources/SQL/JIT/JitExecPage.cxx
    sources/SQL/JIT/HotPathDetector.cxx
    sources/SQL/JIT/X86_64Emitter.cxx
    sources/SQL/JIT/Arm64Emitter.cxx
    sources/SQL/MathSci/MathSciOptimizer.cxx
    sources/SQL/Optimizer/GraphOptimizer.cxx
    sources/SQL/Optimizer/GraphCypher.cxx
    sources/SQL/Parser/Parser.cxx
    sources/SQL/Parser/DialectCompat.cxx
    sources/SQL/Procedures/ProcedureParser.cxx
    sources/SQL/Parser/SetExprEval.cxx
    sources/Database/Index/FtsIndex.cxx
    sources/Database/Index/VectorIndex.cxx
    sources/SQL/Parser/MatchRecognize.cxx
    sources/SQL/Rewrite/RecursiveCteOptimizer.cxx
    sources/SQL/Rewrite/BfsRecursiveExecutor.cxx
    sources/SQL/Rewrite/SubqueryFlattener.cxx
    sources/SQL/Rewrite/SemiJoinRewriter.cxx
    sources/SQL/Rewrite/MagicSetRewriter.cxx
    sources/SQL/Rewrite/LazyCorrelationCache.cxx
    sources/SQL/Rewrite/RewritePipeline.cxx
    sources/SQL/Rewrite/RewriteOps.cxx
    sources/SQL/Profiler/QueryProfiler.cxx
    sources/SQL/Profiler/SqlSessionConfig.cxx
    sources/SQL/Profiler/PragmaHandler.cxx
    sources/SQL/Profiler/ExplainAnalyze.cxx
    sources/SQL/Profiler/SqlPipeline.cxx
    sources/SQL/Profiler/SqlPlanCache.cxx
    sources/SQL/Shape/ShapeTelemetryTypes.cxx
    sources/SQL/Shape/ShapeComposition.cxx
    sources/SQL/Shape/QueryShapeRouter.cxx
    sources/DS/HyperLogLog.cxx
    sources/IO/MmapBytecodeCache.cxx
)

target_include_directories(astraldb_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sources>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(astraldb_core PRIVATE /W4)
    target_link_libraries(astraldb_core PRIVATE cabinet)
    target_compile_definitions(astraldb_core PRIVATE ASTRALDB_HAVE_WIN_LZX=1)
    if(ASTRALDB_WARNINGS_AS_ERRORS)
        target_compile_options(astraldb_core PRIVATE /WX)
    endif()
else()
    target_compile_options(astraldb_core PRIVATE -Wall -Wextra)
    if(ASTRALDB_WARNINGS_AS_ERRORS)
        target_compile_options(astraldb_core PRIVATE -Werror)
    endif()
endif()

if(ASTRALDB_IO_URING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(LIBURING QUIET liburing)
	endif()
	if(LIBURING_FOUND)
		target_compile_definitions(astraldb_core PRIVATE ASTRALDB_HAVE_IO_URING)
		target_link_libraries(astraldb_core PRIVATE ${LIBURING_LIBRARIES})
		target_include_directories(astraldb_core PRIVATE ${LIBURING_INCLUDE_DIRS})
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
set_target_properties(astraldb_c PROPERTIES CXX_VISIBILITY_PRESET default)
astraldb_apply_target_options(astraldb_c)

add_executable(astraldb
    sources/Program.cxx
)
target_link_libraries(astraldb PRIVATE astraldb_core)
target_include_directories(astraldb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
astraldb_apply_executable_options(astraldb)

add_executable(astraldb_tests
    tests/TestMain.cxx
    tests/AstralDB.Tests.cxx
)
target_link_libraries(astraldb_tests PRIVATE astraldb_core astraldb_c)
target_include_directories(astraldb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(astraldb_tests PROPERTIES OUTPUT_NAME run_tests)
astraldb_apply_target_options(astraldb_tests)
if(WIN32)
	target_link_options(astraldb_tests PRIVATE "-Wl,--stack,8388608")
endif()

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
