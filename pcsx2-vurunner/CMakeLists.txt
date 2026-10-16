add_executable(pcsx2-vurunner)

if(PACKAGE_MODE)
	install(TARGETS pcsx2-vurunner DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
	install(TARGETS pcsx2-vurunner DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif()

# vurunner reuses the recompiler test harness's environment + replay driver.
# Both the harness env and StubHost are also linked by the gtest binary; they
# are compiled in here directly (rather than as a shared library) to mirror
# pcsx2-gsrunner's single-binary-no-extra-libs pattern.
target_sources(pcsx2-vurunner PRIVATE
	Main.cpp
	VuAotMode.cpp
	${CMAKE_SOURCE_DIR}/tests/ctest/core/StubHost.cpp
	${CMAKE_SOURCE_DIR}/tests/ctest/core/recompilers/harness/RecompilerTestEnvironment.cpp
	${CMAKE_SOURCE_DIR}/tests/ctest/core/recompilers/harness/VuSnapshot.cpp
	${CMAKE_SOURCE_DIR}/tests/ctest/core/recompilers/harness/VuReplay.cpp
)

target_include_directories(pcsx2-vurunner PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${CMAKE_SOURCE_DIR}/tests/ctest/core/recompilers"
)

target_link_libraries(pcsx2-vurunner PRIVATE
	PCSX2_FLAGS
	PCSX2
)

# A non-PIE executable (ET_EXEC) loads at its fixed link address even with
# ASLR enabled. The persisted-JIT VU program cache used to need that; its
# payloads now relocate (image and data-arena addresses are fixups), and a
# PIE build hydrates them too. vurunner stays non-PIE so its corpus gates
# keep exercising the fixed layout the handheld build (pcsx2-sdl) runs with;
# pcsx2-eerunner and recompiler_tests are PIE and cover the other case.
# `pcsx2-vurunner --print-bases` prints the layout. Linux-only; libpcsx2's
# -fPIC objects link into an ET_EXEC fine.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set_target_properties(pcsx2-vurunner PROPERTIES POSITION_INDEPENDENT_CODE OFF)
	target_link_options(pcsx2-vurunner PRIVATE -no-pie)
endif()
