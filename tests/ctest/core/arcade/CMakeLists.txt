# Namco arcade: the controller-to-JVS mapping per control mode and the player's remap, the hook that gives
# the cabinet the controller, the arcade game list entries and the BIOS lists. Links the whole core like
# core_test does, because ACJV, GameList and the BIOS tools do not stand alone.
add_pcsx2_test(arcade_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
	arcade_bios_tests.cpp
	arcade_files_tests.cpp
	arcade_gamelist_tests.cpp
	arcade_hook_tests.cpp
	arcade_input_tests.cpp
)

# .acgame files the tests read, and the directory the tests write their scratch files under.
target_compile_definitions(arcade_tests PRIVATE
	ARCADE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(arcade_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)
