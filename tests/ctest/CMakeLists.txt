enable_testing()
add_custom_target(unittests)
add_custom_command(TARGET unittests POST_BUILD COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure)

# Upstream excludes every gate binary from the default target, so a plain build
# leaves the gates at whatever revision they were last asked for BY NAME -- which
# is the same structural fault ENABLE_RIG exists to remove for the runners. Under
# ENABLE_RIG they are ordinary members of `all`; without it, nothing changes.
#
# ⚠️ Being reachable through `--target rig` is NOT the same thing and does not
# substitute: the rig doctor demands these be current with the source, so leaving
# them out of `all` makes a correct-looking build fail its own freshness check.
if(ENABLE_RIG)
	set(_pcsx2_test_exclude "")
else()
	set(_pcsx2_test_exclude EXCLUDE_FROM_ALL)
endif()

macro(add_pcsx2_test target)
	add_executable(${target} ${_pcsx2_test_exclude} ${ARGN})
	target_link_libraries(${target} PRIVATE gtest)
	if(APPLE)
		# For some reason this doesn't get pulled in implicitly...
		target_link_libraries(${target} PRIVATE
			"-framework Foundation"
			"-framework Cocoa"
		)
	endif()

	add_dependencies(unittests ${target})
	add_test(NAME ${target} COMMAND ${target})

	# A gate binary is a measurement binary: it answers a question about the
	# tree, so it belongs to `rig` for the same reason the runners do. Kept
	# separate from `unittests`, which RUNS ctest as a post-build step -- `rig`
	# only has to build things.
	if(TARGET rig)
		add_dependencies(rig ${target})
		set_property(GLOBAL APPEND PROPERTY ARMSX2_RIG_TARGETS ${target})
	endif()

	if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		# For some reason, the stack refuses to grow with the latest MSVC updates.
		# Just force the commit size to 1MB for now.
		set_target_properties(${target} PROPERTIES LINK_FLAGS "/STACK:1048576,1048576")
	endif()
endmacro()

add_subdirectory(common)
add_subdirectory(core)
