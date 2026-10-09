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
