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
	gs_copy_road_blending_tests.cpp
	gs_native_texel_grid_tests.cpp
	gs_self_read_road_tests.cpp
	gs_dynamic_feedback_loop_tests.cpp
	gs_stream_ring_memory_tests.cpp
	gs_stream_ring_flush_tests.cpp
	gs_gpu_driver_profile_tests.cpp
	gs_draw_staging_tests.cpp
	gs_draw_env_snapshot_tests.cpp
	gs_snapshot_policy_tests.cpp
	gs_sw_scanline_cpath_tests.cpp
	gs_aa1_edge_tests.cpp
	gs_sw_colour_walk_tables_tests.cpp
	gs_line_walk_tests.cpp
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
	gs_sw_scanline_lod_tests.cpp
	gs_sw_textured_block_width_tests.cpp
	gs_sw_depth_walk_tests.cpp
	gs_sw_span_seed_tests.cpp
	gs_sw_fog_blend_tests.cpp
	gs_local_memory_move_tests.cpp
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
	gs_autoflush_engine_level_tests.cpp
	gs_draw_buffer_overlap_tests.cpp
	gs_ktx_tests.cpp
	gs_texture_cache_grid_tests.cpp
	gs_dither_scale_tests.cpp
	gs_cull_grid_refresh_tests.cpp
	gs_cull_grid_native_target_tests.cpp
	gs_point_safe_features_tests.cpp
	gs_field_shift_detector_tests.cpp
	gs_channel_shuffle_cover_tests.cpp
	gs_sw_palette_block_copy_tests.cpp
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
