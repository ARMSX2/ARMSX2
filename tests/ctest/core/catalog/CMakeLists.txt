# Tests for the shared online-service layer in pcsx2/Catalog/ (fetching, caching, texture-pack
# catalog and installer, covers, driver packs, patches, memory-card backups) plus the SHA-256
# helper in common/. No test here may touch the network: services take a Catalog::Fetcher, and
# the tests hand them a FakeFetcher (FakeFetcher.h) loaded with canned responses.
#
# Every *_tests.cpp in this directory is picked up automatically, so a service adds its tests
# by dropping in a file, with no edit to this list. Re-run cmake once after adding a file; the
# CONFIGURE_DEPENDS glob makes a plain build notice it too.
#
# Fixtures (captured catalog JSON, sample archives, ...) live in fixtures/ and are found at run
# time through CATALOG_FIXTURE_DIR; see catalog_test_support.h.
file(GLOB catalog_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_tests.cpp")

add_pcsx2_test(catalog_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
	${catalog_test_sources}
)

target_include_directories(catalog_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_compile_definitions(catalog_tests PRIVATE
	CATALOG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(catalog_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)

# Shared libraries for driver_packs_tests.cpp. DriverPacks::TestLoadLibrary dlopens a pack's
# driver in a child process; these are the ways that can go: loads fine, a dependency missing,
# an undefined symbol, a constructor that crashes or never returns or finds descriptors it should not
# have, a destructor that crashes.
# They are plain C so loading them needs nothing from the C++ runtime. The project compiles with
# -fvisibility=hidden, so the functions another library looks up by name say so themselves.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set(catalog_test_lib_dir ${CMAKE_CURRENT_BINARY_DIR}/test_libs)
	set(catalog_test_libs)
	foreach(name ok dep needs_dep undefined_symbol ctor_abort ctor_segv ctor_hang ctor_fds dtor_abort)
		string(REPLACE "_" "" lib_file ${name})
		add_library(catalog_test_lib_${name} SHARED ${CMAKE_CURRENT_SOURCE_DIR}/test_libs/${name}.c)
		set_target_properties(catalog_test_lib_${name} PROPERTIES
			OUTPUT_NAME catalogtest${lib_file}
			LIBRARY_OUTPUT_DIRECTORY ${catalog_test_lib_dir}
		)
		list(APPEND catalog_test_libs catalog_test_lib_${name})
	endforeach()
	# needs_dep finds dep next to itself, so the test can copy the pair anywhere.
	target_link_libraries(catalog_test_lib_needs_dep PRIVATE catalog_test_lib_dep)
	set_target_properties(catalog_test_lib_needs_dep PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")

	add_dependencies(catalog_tests ${catalog_test_libs})
	target_compile_definitions(catalog_tests PRIVATE CATALOG_TEST_LIB_DIR="${catalog_test_lib_dir}")
endif()
