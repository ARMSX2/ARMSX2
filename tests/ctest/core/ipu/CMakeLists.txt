# IPU colour-conversion oracle suite. ipu_dither() compiles to a different
# implementation per architecture and the three were never compared to each other;
# these hold whichever one this host selected to the scalar reference.
add_pcsx2_test(ipu_dither_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
	ipu_dither_tests.cpp
)

target_include_directories(ipu_dither_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_link_libraries(ipu_dither_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)
