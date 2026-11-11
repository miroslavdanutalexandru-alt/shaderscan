# shaderscan - offline RSX shader-cache seeding (see README.md)
# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20)
project(shaderscan VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
	# one self-contained exe: no VC++ runtime DLLs needed on the user's PC
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# zlib inflate for PSARC blocks: miniz (MIT), vendored under third_party/miniz
set(SHADERSCAN_MINIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz" CACHE PATH "miniz source dir")

add_library(shaderscan_core STATIC
	src/rsx_program.cpp
	src/programs.cpp
	src/psarc.cpp
	src/rpcs3_cache.cpp
	src/fe_insomniac.cpp
	src/fe_signature.cpp
	src/shaderscan.cpp
	src/ingest.cpp
	src/backend_rpcs3.cpp
	${SHADERSCAN_MINIZ_DIR}/miniz.c)
target_include_directories(shaderscan_core PUBLIC include PRIVATE src ${SHADERSCAN_MINIZ_DIR})
target_compile_definitions(shaderscan_core PRIVATE _CRT_SECURE_NO_WARNINGS MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_APIS)

add_executable(shaderscan src/main.cpp)
target_include_directories(shaderscan PRIVATE src)
target_link_libraries(shaderscan PRIVATE shaderscan_core)
target_compile_definitions(shaderscan PRIVATE _CRT_SECURE_NO_WARNINGS SHADERSCAN_RELEASE="${PROJECT_VERSION}")

# Back end "r1d3d12" (Resistance: Fall of Man port's own D3D12 renderer). It compiles that port's RSX->HLSL
# translators so generated shaders and cache keys match the game exactly. By default the copy vendored under
# third_party/rfom_translators is used; SHADERSCAN_R1_VIDEO_DIR overrides it with a ResistancePort checkout's
# ps3recomp/libs/video (whose rsx_d3d12_backend.c must carry the rfom-shc-v1 cache key). Empty = no r1d3d12.
if(WIN32)
	set(_r1_vendored "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rfom_translators/libs/video")
	set(SHADERSCAN_R1_VIDEO_DIR "${_r1_vendored}" CACHE PATH "Fall of Man RSX->HLSL translator dir (ps3recomp/libs/video layout; empty = no r1d3d12 back end)")
	set(_r1_ok OFF)
	if(SHADERSCAN_R1_VIDEO_DIR AND EXISTS "${SHADERSCAN_R1_VIDEO_DIR}/rsx_fp_decompiler.c")
		get_filename_component(_r1_dir "${SHADERSCAN_R1_VIDEO_DIR}" ABSOLUTE)
		get_filename_component(_r1_vend "${_r1_vendored}" ABSOLUTE)
		if(_r1_dir STREQUAL _r1_vend)
			set(_r1_ok ON)
		elseif(EXISTS "${SHADERSCAN_R1_VIDEO_DIR}/rsx_d3d12_backend.c")
			file(READ "${SHADERSCAN_R1_VIDEO_DIR}/rsx_d3d12_backend.c" _r1_backend_src)
			string(FIND "${_r1_backend_src}" "rfom-shc-v1" _r1_tag)
			if(_r1_tag EQUAL -1)
				message(WARNING "shaderscan: ${SHADERSCAN_R1_VIDEO_DIR} is not the Fall of Man renderer (no rfom-shc-v1 cache key); r1d3d12 back end off")
			else()
				set(_r1_ok ON)
			endif()
		else()
			message(WARNING "shaderscan: ${SHADERSCAN_R1_VIDEO_DIR} has no rsx_d3d12_backend.c; r1d3d12 back end off")
		endif()
	elseif(SHADERSCAN_R1_VIDEO_DIR)
		message(WARNING "shaderscan: no rsx_fp_decompiler.c in ${SHADERSCAN_R1_VIDEO_DIR}; r1d3d12 back end off")
	endif()
	if(_r1_ok)
		add_library(shaderscan_r1_translator STATIC
			${SHADERSCAN_R1_VIDEO_DIR}/rsx_fp_decompiler.c
			${SHADERSCAN_R1_VIDEO_DIR}/rsx_vp_decompiler.c
			src/r1d3d12/r1_glue.c)
		target_include_directories(shaderscan_r1_translator PRIVATE ${SHADERSCAN_R1_VIDEO_DIR} ${SHADERSCAN_R1_VIDEO_DIR}/../../include)
		target_compile_definitions(shaderscan_r1_translator PRIVATE _CRT_SECURE_NO_WARNINGS)
		target_sources(shaderscan_core PRIVATE src/backend_r1d3d12.cpp)
		set_source_files_properties(src/backend_r1d3d12.cpp PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/profiles/r1_profile.inc")
		target_compile_definitions(shaderscan_core PUBLIC SHADERSCAN_HAVE_R1D3D12=1)
		target_link_libraries(shaderscan_core PUBLIC shaderscan_r1_translator)
		message(STATUS "shaderscan: r1d3d12 back end from ${SHADERSCAN_R1_VIDEO_DIR}")
	endif()
endif()

install(TARGETS shaderscan RUNTIME DESTINATION .)
