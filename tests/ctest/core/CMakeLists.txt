add_pcsx2_test(core_test
	ee_fpu_regfile_tests.cpp
	ee_clamp_mode_tests.cpp
	vu_clamp_mode_tests.cpp
	vuaot_setting_tests.cpp
	ee_fpu_reloc_tests.cpp
	patch_tests.cpp
	autotune_applier_tests.cpp
	autotune_controller_tests.cpp
	autotune_sampler_tests.cpp
	autotune_settings_tests.cpp
	autotune_store_tests.cpp
	autotune_trace_tests.cpp
	iop_fingerprint_tests.cpp
	iop_thread_mode_tests.cpp
	iop_thread_placement_tests.cpp
	iop_thread_metrics_tests.cpp
	iop_thread_channels_tests.cpp
	iop_thread_tests.cpp
	iop_thread_lockstep_tests.cpp
	iop_thread_queue_tests.cpp
	iop_thread_hold_tests.cpp
	iop_thread_owner_tests.cpp
	iop_thread_owner_release_tests.cpp
	iop_thread_host_commands_tests.cpp
	iop_thread_host_calls_tests.cpp
	iop_thread_fpcr_tests.cpp
	iop_thread_threaded_tests.cpp
	pad_input_tests.cpp
	shared_word_tests.cpp
	IopThreadTestSupport.h
	StubHostTasks.h
	sif_pump_tests.cpp
	sbus_tests.cpp
	savestate_legacy_tests.cpp
	settings_precedence_tests.cpp
	gsrunner_affinity_tests.cpp
	headless_shot_tests.cpp
	onscreen_keyboard_tests.cpp
	onscreen_keyboard_ui_tests.cpp
	texture_upscale_setting_tests.cpp
	ee_cycle_rate_controller_tests.cpp
	ee_cycle_rate_eligibility_tests.cpp
	ee_cycle_rate_sampler_tests.cpp
	MockMemoryInterface.h
	StubHost.cpp
)

# gsrunner_affinity_tests.cpp includes the runner's own -affinity parse header; the flag
# decides which cores a measurement round runs on, so it is worth a test even though the
# runner itself has no test binary.
target_include_directories(core_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../pcsx2-gsrunner
)

if(ARCH_ARM64)
	# Phase 0.6: ARM64 JIT toolchain smoke test (emit + execute ARM64 code).
	target_sources(core_test PRIVATE arm64_emit_test.cpp)
	target_sources(core_test PRIVATE arm64_lane_gather_tests.cpp)
endif()

# GSLsfg.cpp only builds with the Vulkan renderer, so its structural-PE-check test follows it.
if(USE_VULKAN)
	target_sources(core_test PRIVATE lsfg_dll_check_tests.cpp)
endif()

# The recompiler differential-test harness drives the arm64 JIT through
# arm64-only entry points (recEeExecuteBlock, recEeIsBlockLinked) and the
# arm64 microVU persist API, so it only builds on ARCH_ARM64.
if(ARCH_ARM64)
	add_subdirectory(recompilers)
endif()

# GS vertex front-end kernel oracle (arch-neutral).
add_subdirectory(gs)

# IPU colour-conversion oracle (arch-neutral: each host tests its own path).
add_subdirectory(ipu)

# Shared online-service layer (pcsx2/Catalog) and common/SHA256.
add_subdirectory(catalog)

# Namco arcade cabinet controls and library entries.
add_subdirectory(arcade)


set(multi_isa_sources
	GS/swizzle_test_main.cpp
)

target_link_libraries(core_test PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)

if(DISABLE_ADVANCE_SIMD AND ARCH_X86)
	if(WIN32)
		set(compile_options_avx2 /arch:AVX2)
		set(compile_options_avx  /arch:AVX)
	elseif(USE_GCC)
		# GCC can't inline into multi-isa functions if we use march and mtune, but can if we use feature flags
		set(compile_options_avx2 -msse4.1 -mavx -mavx2 -mbmi -mbmi2 -mfma)
		set(compile_options_avx  -msse4.1 -mavx)
		set(compile_options_sse4 -msse4.1)
	else()
		set(compile_options_avx2 -march=haswell -mtune=haswell)
		set(compile_options_avx  -march=sandybridge -mtune=sandybridge)
		set(compile_options_sse4 -msse4.1 -mtune=nehalem)
	endif()

	# This breaks when running on Apple Silicon, because even though we skip the test itself, the
	# gtest constructor still generates AVX code, and that's a global object which gets constructed
	# at binary load time. So, for now, only compile SSE4 if running on ARM64.
	if (NOT APPLE OR "${CMAKE_HOST_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
		set(isa_list "sse4" "avx" "avx2")
	else()
		set(isa_list "sse4")
	endif()

	# ODR violation time!
	# Everything would be fine if we only defined things in cpp files, but C++ tends to like inline functions (STL anyone?)
	# Each ISA will bring with it its own copies of these inline header functions, and the linker gets to choose whichever one it wants!  Not fun if the linker chooses the avx2 version and uses it with everything
	# Thankfully, most linkers don't choose at random.  When presented with a bunch of .o files, most linkers seem to choose the first implementation they see, so make sure you order these from oldest to newest
	# Note: ld64 (macOS's linker) does not act the same way when presented with .a files, unless linked with `-force_load` (cmake WHOLE_ARCHIVE).
	set(is_first_isa "1")
	foreach(isa IN LISTS isa_list)
		add_library(core_test_${isa} STATIC ${multi_isa_sources})
		target_link_libraries(core_test_${isa} PRIVATE PCSX2_FLAGS gtest)
		target_compile_definitions(core_test_${isa} PRIVATE MULTI_ISA_UNSHARED_COMPILATION=isa_${isa} MULTI_ISA_IS_FIRST=${is_first_isa} ${pcsx2_defs_${isa}})
		target_compile_options(core_test_${isa} PRIVATE ${compile_options_${isa}})
		if (${CMAKE_VERSION} VERSION_GREATER_EQUAL 3.24)
			target_link_libraries(core_test PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,core_test_${isa}>)
		elseif(APPLE)
			message(FATAL_ERROR "MacOS builds with DISABLE_ADVANCE_SIMD=ON require CMake 3.24")
		else()
			target_link_libraries(core_test PRIVATE core_test_${isa})
		endif()
		set(is_first_isa "0")
	endforeach()
else()
	target_sources(core_test PRIVATE ${multi_isa_sources})
endif()

if(WIN32 AND TARGET SDL3::SDL3)
	# Copy SDL3 DLL to binary directory.
	if(CMAKE_BUILD_TYPE STREQUAL "Debug")
		get_property(SDL3_DLL_PATH TARGET SDL3::SDL3 PROPERTY IMPORTED_LOCATION_DEBUG)
	else()
		get_property(SDL3_DLL_PATH TARGET SDL3::SDL3 PROPERTY IMPORTED_LOCATION_RELEASE)
	endif()
	if(SDL3_DLL_PATH)
		add_custom_command(TARGET core_test POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:core_test>"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SDL3_DLL_PATH}" "$<TARGET_FILE_DIR:core_test>")
	endif()
endif()
