# SPU2: the mixer against a frozen copy of itself. Links the whole core like core_test does, because the mixer
# reads the global SPU2 state, the IOP interrupt registers and the reverb filters.
add_pcsx2_test(spu2_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
	spu2_mixer_current.cpp
	spu2_mixer_reference.cpp
	spu2_mixer_tests.cpp
	spu2_mixer_reference.inc
)

target_link_libraries(spu2_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)
