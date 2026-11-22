add_pcsx2_test(common_test
	byteswap_tests.cpp
	filesystem_tests.cpp
	host_sys_tests.cpp
	path_tests.cpp
	pmu_counters_tests.cpp
	small_string_tests.cpp
	string_util_tests.cpp
	texture_decode_tests.cpp
)

if(ARCH_X86)
	target_sources(common_test PRIVATE
		x86emitter/codegen_tests.cpp
		x86emitter/codegen_tests.h
		x86emitter/codegen_tests_main.cpp
	)
endif()

target_link_libraries(common_test PRIVATE
	common
)

if(ARCH_ARM64)
	add_pcsx2_test(host_page_test
		host_sys_tests.cpp
		../core/recompilers/mvu_jump_table_tests.cpp
		${CMAKE_SOURCE_DIR}/pcsx2/arm64/microVU_JumpTable-arm64.cpp
	)
	target_include_directories(host_page_test PRIVATE ${CMAKE_SOURCE_DIR}/pcsx2)
	target_link_libraries(host_page_test PRIVATE common)
endif()
