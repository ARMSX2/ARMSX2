# Oracle suite for the GS vertex front-end kernels (GS/GSVertexKick.h): the
# legacy parse and accept/cull kernels are pinned against independent scalar
# models over directed edges + randomized sweeps. GV-campaign optimized kernels
# must pass the same suite bit-for-bit. See
# scratchpad/mq65-gs-2026-07-18/GIFPACKED-VERTEX-DESIGN.md for the campaign.
#
# The *_policy_tests.cpp, road and profile test files ride this target rather
# than adding one: what they test is header-only, so they need no extra linkage.
add_pcsx2_test(gs_vertex_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
	gs_vertex_tests.cpp
	gs_kick_kernel_tests.cpp
	gs_backqueue_tests.cpp
	gs_back_thread_policy_tests.cpp
	gs_interlace_policy_tests.cpp
	gs_field_shift_policy_tests.cpp
	gs_framebuffer_fetch_policy_tests.cpp
	gs_draw_road_tests.cpp
	gs_date_road_tests.cpp
	gs_alpha_bit_logic_op_tests.cpp
	gs_replacement_alpha_snap_tests.cpp
	gs_copy_road_blending_tests.cpp
	gs_native_texel_grid_tests.cpp
	gs_self_read_road_tests.cpp
	gs_dynamic_feedback_loop_tests.cpp
	gs_stream_ring_memory_tests.cpp
	gs_stream_ring_flush_tests.cpp
	gs_stream_ring_growth_tests.cpp
	gs_gpu_driver_profile_tests.cpp
	gs_driver_report_tests.cpp
	gs_vulkan_device_rules_tests.cpp
	gs_gpu_profile_tests.cpp
	gs_draw_staging_tests.cpp
	gs_draw_env_snapshot_tests.cpp
	gs_snapshot_policy_tests.cpp
	gs_sw_scanline_cpath_tests.cpp
	gs_aa1_edge_tests.cpp
	gs_sw_colour_walk_tables_tests.cpp
	gs_line_walk_tests.cpp
	gs_line_runs_tests.cpp
	gs_point_place_tests.cpp
	gs_sw_setup_reciprocal_tests.cpp
	gs_readback_transfer_size_tests.cpp
	gs_astc_tests.cpp
	gs_sw_scanline_date_tests.cpp
	gs_sw_scanline_dither_tests.cpp
	gs_sw_modulate16_tests.cpp
	gs_sw_scanline_texfunc_tests.cpp
	gs_sw_coverage_lag_tests.cpp
	gs_sw_vertex_qdivide_tests.cpp
	gs_sw_perspective_recip_tests.cpp
	gs_sw_scanline_tclag_tests.cpp
	gs_sw_stq_blit_tests.cpp
	gs_sw_coordinate_field_tests.cpp
	gs_sw_coordinate_grain_tests.cpp
	gs_sw_sprite_ramp_tests.cpp
	gs_sw_perspective_plane_tests.cpp
	gs_sw_perspective_plane_scanline_tests.cpp
	gs_sw_scanline_lod_tests.cpp
	gs_sw_textured_block_width_tests.cpp
	gs_sw_depth_walk_tests.cpp
	gs_sw_span_seed_tests.cpp
	gs_sw_fog_blend_tests.cpp
	gs_local_memory_move_tests.cpp
	gs_local_memory_write_stamp_tests.cpp
	gs_write_stamp_writer_tests.cpp
	gs_hash_memo_tests.cpp
	gs_clut_tests.cpp
	gs_alpha_known_bits_tests.cpp
	gs_depth_coverage_tests.cpp
	gs_draw_alpha_mask_tests.cpp
	gs_fast_stencil_shadow_tests.cpp
	gs_sprite_cover_tests.cpp
	gs_sprite_edge_snap_tests.cpp
	gs_sprite_pass_order_tests.cpp
	gs_pass_scheduler_tests.cpp
	gs_null_device_profile_tests.cpp
	gs_sw_texture_dirty_tests.cpp
	gs_blend_constant_policy_tests.cpp
	gs_over_one_two_pass_tests.cpp
	gs_autoflush_engine_level_tests.cpp
	gs_draw_buffer_overlap_tests.cpp
	gs_ktx_tests.cpp
	gs_texture_cache_grid_tests.cpp
	gs_texture_cache_move_tests.cpp
	gs_target_update_tests.cpp
	gs_dither_scale_tests.cpp
	gs_cull_grid_refresh_tests.cpp
	gs_split_seam_tests.cpp
	gs_cull_grid_native_target_tests.cpp
	gs_point_safe_features_tests.cpp
	gs_field_shift_detector_tests.cpp
	gs_channel_shuffle_cover_tests.cpp
	gs_sw_palette_block_copy_tests.cpp
	gs_cache_file_tests.cpp
	gs_hash_cache_inject_tests.cpp
	gs_xxh_block_hasher_tests.cpp
	gs_texture_replacement_worker_tests.cpp
)

target_include_directories(gs_vertex_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_link_libraries(gs_vertex_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)

# The palette block copy against the generated scanline, which is what games run. The same source
# rides gs_vertex_tests too, where there is no code memory and the rasterizer uses its C++ scanline.
# It is its own binary because reserving code memory switches every later rasterizer in the
# process to generated code, which the other suites in gs_vertex_tests do not expect.
add_pcsx2_test(gs_sw_palette_block_copy_jit_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
	gs_sw_palette_block_copy_tests.cpp
)

target_compile_definitions(gs_sw_palette_block_copy_jit_tests PRIVATE GS_PALETTE_COPY_TESTS_JIT=1)

target_include_directories(gs_sw_palette_block_copy_jit_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_link_libraries(gs_sw_palette_block_copy_jit_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)

# The GS cache store under a host file system (the libretro core's VFS). Its own binary: a host file
# system is installed once per process, and would change every other test's files.
if(UNIX)
	add_pcsx2_test(gs_cache_file_vfs_tests
		${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
		gs_cache_file_vfs_tests.cpp
	)
	target_include_directories(gs_cache_file_vfs_tests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
	)
	target_link_libraries(gs_cache_file_vfs_tests PUBLIC
		PCSX2_FLAGS
		PCSX2
		common
	)
endif()

# The dump packer needs only the file system helpers and libzip, so its tests compile it directly
# rather than linking the emulator.
add_pcsx2_test(gs_dump_bundle_tests
	gs_dump_bundle_tests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2/GS/GSDumpBundle.cpp
)

target_include_directories(gs_dump_bundle_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_link_libraries(gs_dump_bundle_tests PUBLIC
	PCSX2_FLAGS
	common
	libzip::zip
)

# The RAISR texture upscaler engine has no GS, GPU or settings dependencies, so its tests compile
# it directly rather than linking the emulator: this builds in seconds and shows the engine
# stands alone. It reads the real filters from bin/resources/upscale/raisr.
set(raisr_engine_source ${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2/GS/Renderers/HW/GSTextureUpscaler.cpp)
add_pcsx2_test(gs_texture_upscaler_tests
	gs_texture_upscaler_tests.cpp
	${raisr_engine_source}
)

target_include_directories(gs_texture_upscaler_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
	${CMAKE_CURRENT_SOURCE_DIR}/../../../..
)

target_compile_definitions(gs_texture_upscaler_tests PRIVATE
	GS_UPSCALER_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../../bin/resources/upscale/raisr"
)

# The upscaler's job queue, scale rule, 2x-twice chaining and mip chain builder are header only and
# need nothing from the emulator beyond the engine, which is compiled in here as well. The chaining
# tests read the real filters, like the engine tests.
add_pcsx2_test(gs_texture_upscale_support_tests
	gs_texture_upscale_support_tests.cpp
	${raisr_engine_source}
)

target_include_directories(gs_texture_upscale_support_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
	${CMAKE_CURRENT_SOURCE_DIR}/../../../..
)

target_compile_definitions(gs_texture_upscale_support_tests PRIVATE
	GS_UPSCALER_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../../bin/resources/upscale/raisr"
)

# Same as pcsx2/CMakeLists.txt: the scalar and NEON paths only match bit for bit if nothing is
# contracted beyond the explicit fused multiply-adds.
if(NOT MSVC)
	set_source_files_properties(${raisr_engine_source} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
