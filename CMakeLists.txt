# Setting it to a range tells it that it supports the features on the newer
# versions as well, avoiding setting policies.
cmake_minimum_required(VERSION 3.16...3.25)

# Enabling this cmake policy gets rid of warnings regarding LTO.
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
	message(FATAL_ERROR "PCSX2 does not support in-tree builds.  Please make a build directory that is not the PCSX2 source directory and generate your CMake project there using either `cmake -B build_directory` or by running cmake from the build directory.")
endif()

# Project Name
project(Pcsx2 C CXX)

# Variable to check that people use the good file
set(TOP_CMAKE_WAS_SOURCED TRUE)

# set module path
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# Write binaries to the bin directory under the build directory.
# This makes it simple to run development builds directly out of the build directory.
if (UNIX AND NOT APPLE)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
endif()

# include some generic functions to ensure correctness of the env
include(Pcsx2Utils)

check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()

#-------------------------------------------------------------------------------
# Include specific module
include(BuildParameters)
include(SearchForStuff)

# Must be done after SearchForStuff
get_git_version_info()
write_svnrev_h()

# The shader tree is SYMLINKED into the runtime resources directory rather than
# copied into it.
#
# pcsx2_copy_runtime_resources() stages bin/resources/ next to a built frontend
# with a POST_BUILD copy_if_different over a configure-time glob. That is fine
# for data nobody edits, and wrong for shaders, which are edited constantly and
# on their own: editing a .glsl compiles nothing, so the frontend does not
# relink, so the POST_BUILD step never runs, so the binary keeps executing the
# previously staged copy. It builds, links and runs clean the whole time -- the
# failure mode is a plausible wrong image, not an error -- and a shader that
# fails to compile does not fail the run either, so the output is not evidence
# the edit arrived. A symlink cannot hold a previous revision.
#
# Only the shader subtree. The staged resources directory also carries files
# that are deliberately NOT the source tree's (the mobile GameDB overlay lands
# flat there, and docs/ comes from bin/docs/), so linking the whole directory
# would break them.
if(UNIX AND NOT APPLE)
	set(_rig_shader_link "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/resources/shaders")
	file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/resources")
	if(IS_DIRECTORY "${_rig_shader_link}" AND NOT IS_SYMLINK "${_rig_shader_link}")
		file(REMOVE_RECURSE "${_rig_shader_link}")
	endif()
	file(CREATE_LINK "${CMAKE_SOURCE_DIR}/bin/resources/shaders" "${_rig_shader_link}"
		SYMBOLIC)
	unset(_rig_shader_link)
endif()

# `rig` -- everything a measurement can run, as one target.
#
# Declared before the subdirectories so each can add itself. Its value is not
# convenience: a build invocation that names targets by hand omits whichever one
# was forgotten, silently and with no signal, and the binary left behind still
# runs. Naming the set once, in the build system, is what makes "did I rebuild
# everything this experiment touches" answerable rather than remembered.
add_custom_target(rig)

# make common
add_subdirectory(common)

# make pcsx2
add_subdirectory(pcsx2)

if(ENABLE_QT_UI)
	add_subdirectory(pcsx2-qt)
endif()

# Updater is Windows only for now.
if (WIN32)
	add_subdirectory(updater)
endif()

# tests
if(ENABLE_TESTS)
	add_subdirectory(3rdparty/googletest)
	add_subdirectory(tests/ctest)
endif()

# gsrunner
if(ENABLE_GSRUNNER)
	add_subdirectory(pcsx2-gsrunner)
else()
	add_subdirectory(pcsx2-gsrunner EXCLUDE_FROM_ALL)
endif()

# eerunner — headless EE (R5900) JIT-vs-interpreter divergence localizer.
# Built on demand (`--target pcsx2-eerunner`); EXCLUDE_FROM_ALL otherwise.
if(ENABLE_EERUNNER)
	add_subdirectory(pcsx2-eerunner)
else()
	add_subdirectory(pcsx2-eerunner EXCLUDE_FROM_ALL)
endif()

# vurunner — headless VU microprogram replayer for codegen iteration. Needs
# the recompiler test hooks since it shares the harness's plumbing.
if(ENABLE_VURUNNER)
	if(NOT ENABLE_RECOMPILER_TEST_HOOKS)
		message(FATAL_ERROR "ENABLE_VURUNNER requires ENABLE_RECOMPILER_TEST_HOOKS=ON.")
	endif()
	add_subdirectory(pcsx2-vurunner)
else()
	add_subdirectory(pcsx2-vurunner EXCLUDE_FROM_ALL)
endif()

# SDL3 / kmsdrm frontend (handheld target)
if(UNIX AND NOT APPLE)
	if(ENABLE_SDL_FRONTEND)
		add_subdirectory(pcsx2-sdl)
	else()
		add_subdirectory(pcsx2-sdl EXCLUDE_FROM_ALL)
	endif()
endif()

# libretro core (armsx2_libretro.so) — opt-in only. When ENABLE_LIBRETRO is OFF
# (the default) the subdirectory is not added at all, so neither the target nor
# its install(TARGETS) rule exist. Adding it EXCLUDE_FROM_ALL instead keeps the
# target out of the default `all`, but its unconditional install() rule still
# drags the core into install-driven builds (ninja/make install, packaging) and
# "build all targets" flows (IDEs) — which is how a plain SDL build ended
# up compiling libretro. Build it with -DENABLE_LIBRETRO=ON.
if(UNIX AND NOT APPLE AND ENABLE_LIBRETRO)
	add_subdirectory(pcsx2-libretro)
endif()

#-------------------------------------------------------------------------------
# Populate `rig`. The unit-test executables add themselves from add_pcsx2_test();
# these are the frontends and headless runners, each guarded because which of
# them exist depends on the configuration.
foreach(_rig_target pcsx2-qt pcsx2-sdl pcsx2-gsrunner pcsx2-eerunner pcsx2-vurunner)
	if(TARGET ${_rig_target})
		add_dependencies(rig ${_rig_target})
		set_property(GLOBAL APPEND PROPERTY ARMSX2_RIG_TARGETS ${_rig_target})
	endif()
endforeach()
unset(_rig_target)

# Write the membership out for tools/rig/doctor.sh.
#
# The doctor needs the CONCRETE targets, not the `rig` alias: a CMake custom
# target carries no output, so it is always dirty and a dry run of it can never
# report "nothing to do". Dry-running the executables themselves is what makes
# freshness decidable. Generating the list here also means the doctor cannot
# drift out of step with the build -- it never holds its own copy of the set.
get_property(_rig_all GLOBAL PROPERTY ARMSX2_RIG_TARGETS)
list(SORT _rig_all)
set(_rig_lines "")
foreach(_t IN LISTS _rig_all)
	# Both columns are needed and neither can be derived from the other: ninja
	# is driven by the target name, and the artifact to hash is the output file,
	# which several targets rename (pcsx2-qt builds armsx2-qt).
	string(APPEND _rig_lines "${_t}\t$<TARGET_FILE:${_t}>\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/rig-targets.txt" CONTENT "${_rig_lines}")
unset(_rig_all)
unset(_rig_lines)
unset(_t)

# `rig-check` -- prove the rig is the tree before measuring with it.
#
# The check that matters is the freshness one, and it is delegated to ninja's own
# dependency graph rather than reasoned about from timestamps: a dry run either
# has work to do or does not. That distinguishes "built" from "built from THIS
# source", which no property of the binary alone can. See tools/rig/doctor.sh.
if(UNIX AND NOT APPLE)
	add_custom_target(rig-check
		COMMAND "${CMAKE_SOURCE_DIR}/tools/rig/doctor.sh"
			--source-dir "${CMAKE_SOURCE_DIR}"
			--build-dir "${CMAKE_BINARY_DIR}"
		COMMENT "Checking the measurement rig is current with the source tree"
		USES_TERMINAL
		VERBATIM)
endif()

if(NOT IS_SUPPORTED_COMPILER)
	message(WARNING "
*************** UNSUPPORTED CONFIGURATION ***************
You are not compiling PCSX2 with a supported compiler.
It may not even build successfully.
PCSX2 only supports the Clang and MSVC compilers.
No support will be provided, continue at your own risk.
*********************************************************")
endif()

if(ARCH_ARM64)
	message(STATUS "ARM64 build: EE, IOP, and VU0/VU1 recompilers are all available.")
endif()
