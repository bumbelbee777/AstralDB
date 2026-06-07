# AstralDB release profiles, SIMD, LTO, link scripts, and UPX helpers.

option(ASTRALDB_LTO "Enable link-time optimization on Release builds" ON)
option(ASTRALDB_STRIP "Strip symbol tables from Release executables" ON)
option(ASTRALDB_UPX "Post-pack Release executables with UPX (release staging)" OFF)
option(ASTRALDB_RELEASE_DIST "Maximum strip/size tuning for shipped release CLI binaries" OFF)
option(ASTRALDB_NATIVE_ARCH "Use -march=native or /arch:AVX2 on speed profile (x86)" OFF)

set(ASTRALDB_RELEASE_PROFILE "size" CACHE STRING "Release tuning: size (default) or speed")
set_property(CACHE ASTRALDB_RELEASE_PROFILE PROPERTY STRINGS size speed)

set(ASTRALDB_SIMD "auto" CACHE STRING "SIMD ISA: auto, off, avx512, avx2, sse42, sse2, neon, sve")
set_property(CACHE ASTRALDB_SIMD PROPERTY STRINGS auto off avx512 avx2 sse42 sse2 neon sve)

set(ASTRALDB_LINK_DIR "${CMAKE_CURRENT_LIST_DIR}/link")

function(astraldb_is_release_config out_var)
	if(CMAKE_CONFIGURATION_TYPES)
		set(${out_var} "$<CONFIG:Release>" PARENT_SCOPE)
	else()
		if(CMAKE_BUILD_TYPE STREQUAL "Release")
			set(${out_var} TRUE PARENT_SCOPE)
		else()
			set(${out_var} FALSE PARENT_SCOPE)
		endif()
	endif()
endfunction()

function(astraldb_apply_release_profile target)
	if(MSVC)
		if(ASTRALDB_RELEASE_PROFILE STREQUAL "speed")
			target_compile_options(${target} PRIVATE
				"$<$<CONFIG:Release>:/O2>")
		else()
			target_compile_options(${target} PRIVATE
				"$<$<CONFIG:Release>:/O1>"
				"$<$<CONFIG:Release>:/Gy>"
				"$<$<CONFIG:Release>:/Gw>")
		endif()
		target_link_options(${target} PRIVATE
			"$<$<CONFIG:Release>:/OPT:REF>"
			"$<$<CONFIG:Release>:/OPT:ICF>"
			"$<$<CONFIG:Release>:/INCREMENTAL:NO>"
			"$<$<CONFIG:Release>:/PDB:NONE>")
	else()
		if(ASTRALDB_RELEASE_PROFILE STREQUAL "speed")
			target_compile_options(${target} PRIVATE
				"$<$<CONFIG:Release>:-O3>")
		else()
			if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
				target_compile_options(${target} PRIVATE
					"$<$<CONFIG:Release>:-Oz>"
					"$<$<CONFIG:Release>:-g0>")
			else()
				target_compile_options(${target} PRIVATE
					"$<$<CONFIG:Release>:-Os>"
					"$<$<CONFIG:Release>:-g0>")
			endif()
		endif()
		target_compile_options(${target} PRIVATE
			"$<$<CONFIG:Release>:-ffunction-sections>"
			"$<$<CONFIG:Release>:-fdata-sections>"
			"$<$<CONFIG:Release>:-fvisibility=hidden>"
			"$<$<CONFIG:Release>:-fmerge-all-constants>"
			"$<$<CONFIG:Release>:-fno-unroll-loops>"
			"$<$<CONFIG:Release>:-fno-unwind-tables>"
			"$<$<CONFIG:Release>:-fno-asynchronous-unwind-tables>"
			"$<$<CONFIG:Release>:-fno-ident>"
			"$<$<CONFIG:Release>:-g0>")
		if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
			target_compile_options(${target} PRIVATE
				"$<$<CONFIG:Release>:-fno-semantic-interposition>")
		endif()
		if(APPLE)
			target_link_options(${target} PRIVATE
				"$<$<CONFIG:Release>:-Wl,-dead_strip>")
		else()
			target_link_options(${target} PRIVATE
				"$<$<CONFIG:Release>:-Wl,--gc-sections>"
				"$<$<CONFIG:Release>:-Wl,-O2>")
			if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
				target_link_options(${target} PRIVATE
					"$<$<CONFIG:Release>:-Wl,--icf=safe>")
			endif()
		endif()
	endif()
endfunction()

function(astraldb_apply_simd target)
	if(ASTRALDB_SIMD STREQUAL "off")
		return()
	endif()
	if(ASTRALDB_SIMD STREQUAL "avx512")
		if(MSVC)
			target_compile_options(${target} PRIVATE /arch:AVX512)
		else()
			target_compile_options(${target} PRIVATE -mavx512f -mfma)
		endif()
		return()
	endif()
	if(ASTRALDB_SIMD STREQUAL "avx2")
		if(MSVC)
			target_compile_options(${target} PRIVATE /arch:AVX2)
		else()
			target_compile_options(${target} PRIVATE -mavx2 -mfma)
		endif()
		return()
	endif()
	if(ASTRALDB_SIMD STREQUAL "sse42")
		if(MSVC)
			target_compile_options(${target} PRIVATE /arch:SSE4.2)
		else()
			target_compile_options(${target} PRIVATE -msse4.2)
		endif()
		return()
	endif()
	if(ASTRALDB_SIMD STREQUAL "sse2")
		if(MSVC)
			target_compile_options(${target} PRIVATE /arch:SSE2)
		else()
			target_compile_options(${target} PRIVATE -msse2)
		endif()
		return()
	endif()
	if(ASTRALDB_SIMD STREQUAL "neon")
		return()
	endif()
	if(ASTRALDB_SIMD STREQUAL "sve")
		if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM64")
			if(MSVC)
				target_compile_options(${target} PRIVATE /arch:armv8.2-a+sve)
			else()
				target_compile_options(${target} PRIVATE -march=armv8-a+sve)
			endif()
		endif()
		return()
	endif()
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM64")
		return()
	endif()
	if(ASTRALDB_RELEASE_PROFILE STREQUAL "speed" AND ASTRALDB_NATIVE_ARCH)
		if(MSVC)
			target_compile_options(${target} PRIVATE /arch:AVX2)
		else()
			target_compile_options(${target} PRIVATE -march=native)
		endif()
	elseif(NOT MSVC)
		target_compile_options(${target} PRIVATE -mavx2 -mfma)
	endif()
endfunction()

function(astraldb_apply_lto target)
	if(NOT ASTRALDB_LTO)
		return()
	endif()
	if(MSVC)
		set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
		target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:/GL>")
		target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:/LTCG>")
	elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:-flto=thin>")
		target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:-flto=thin>")
	elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:-flto>")
		target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:-flto>")
	endif()
endfunction()

function(astraldb_apply_strip target)
	if(NOT ASTRALDB_STRIP)
		return()
	endif()
	if(MSVC)
		return()
	endif()
	if(APPLE)
		target_link_options(${target} PRIVATE
			"$<$<CONFIG:Release>:-Wl,-x>"
			"$<$<CONFIG:Release>:-Wl,-S>")
	else()
		target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:-Wl,-s>")
	endif()
endfunction()

function(astraldb_apply_link_script target)
	if(NOT ASTRALDB_RELEASE_PROFILE STREQUAL "size")
		return()
	endif()
	if(APPLE)
		return()
	endif()
	if(MSVC)
		target_link_options(${target} PRIVATE
			"$<$<CONFIG:Release>:/FILEALIGN:512>")
		return()
	endif()
	set(_elf_ld "${ASTRALDB_LINK_DIR}/astraldb-size.ld")
	if(EXISTS "${_elf_ld}" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
		if(NOT WIN32)
			target_link_options(${target} PRIVATE
				"$<$<CONFIG:Release>:-Wl,-T,${_elf_ld}>")
		else()
			target_link_options(${target} PRIVATE
				"$<$<CONFIG:Release>:-Wl,--file-alignment,512>")
		endif()
	endif()
endfunction()

function(astraldb_apply_llvm_strip target)
	if(NOT ASTRALDB_STRIP OR MSVC)
		return()
	endif()
	if(NOT TARGET ${target})
		return()
	endif()
	get_target_property(_kind ${target} TYPE)
	if(NOT _kind STREQUAL "EXECUTABLE")
		return()
	endif()
	find_program(LLVM_STRIP NAMES llvm-strip strip)
	if(NOT LLVM_STRIP)
		return()
	endif()
	if(WIN32 OR APPLE)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${LLVM_STRIP} --strip-all "$<TARGET_FILE:${target}>"
			COMMENT "llvm-strip ${target}"
			VERBATIM)
	else()
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${LLVM_STRIP}
			        --strip-all
			        --remove-section=.comment
			        --remove-section=.note
			        --remove-section=.note.gnu.build-id
			        "$<TARGET_FILE:${target}>"
			COMMENT "llvm-strip ${target}"
			VERBATIM)
	endif()
endfunction()

function(astraldb_apply_upx_pack target)
	if(NOT ASTRALDB_UPX)
		return()
	endif()
	if(NOT TARGET ${target})
		return()
	endif()
	get_target_property(_kind ${target} TYPE)
	if(NOT _kind STREQUAL "EXECUTABLE")
		return()
	endif()
	if(APPLE)
		return()
	endif()
	set(_pack_sh "${CMAKE_SOURCE_DIR}/scripts/pack_release_binary.sh")
	set(_pack_ps1 "${CMAKE_SOURCE_DIR}/scripts/pack_release_binary.ps1")
	if(WIN32)
		if(NOT EXISTS "${_pack_ps1}")
			return()
		endif()
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${_pack_ps1}"
			        -InputPath "$<TARGET_FILE:${target}>"
			COMMENT "UPX pack ${target}"
			VERBATIM)
	else()
		if(NOT EXISTS "${_pack_sh}")
			return()
		endif()
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${_pack_sh}" "$<TARGET_FILE:${target}>"
			COMMENT "UPX pack ${target}"
			VERBATIM)
	endif()
endfunction()

function(astraldb_apply_target_options target)
	astraldb_apply_release_profile(${target})
	astraldb_apply_simd(${target})
	astraldb_apply_lto(${target})
	astraldb_apply_strip(${target})
endfunction()

function(astraldb_apply_executable_options target)
	astraldb_apply_target_options(${target})
	astraldb_apply_link_script(${target})
	astraldb_apply_llvm_strip(${target})
	astraldb_apply_upx_pack(${target})
endfunction()

# Call once at configure time (e.g. -DASTRALDB_RELEASE_DIST=ON on release CI jobs).
macro(astraldb_configure_release_dist)
	if(ASTRALDB_RELEASE_DIST)
		set(ASTRALDB_RELEASE_PROFILE "size" CACHE STRING "Release tuning: size or speed" FORCE)
		set(ASTRALDB_LTO ON CACHE BOOL "Enable link-time optimization on Release builds" FORCE)
		set(ASTRALDB_STRIP ON CACHE BOOL "Strip symbol tables from Release executables" FORCE)
	endif()
endmacro()
