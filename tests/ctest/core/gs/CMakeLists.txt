# Oracle suite for the GS vertex front-end kernels (GS/GSVertexKick.h): the
# legacy parse and accept/cull kernels are pinned against independent scalar
# models over directed edges + randomized sweeps. GV-campaign optimized kernels
# must pass the same suite bit-for-bit. See
# scratchpad/mq65-gs-2026-07-18/GIFPACKED-VERTEX-DESIGN.md for the campaign.
#
# gs_interlace_policy_tests.cpp and gs_framebuffer_fetch_policy_tests.cpp ride this target rather
# than adding one: those policies are header-only constexpr, so they need no extra linkage.
add_pcsx2_test(gs_vertex_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
	gs_vertex_tests.cpp
	gs_backqueue_tests.cpp
	gs_interlace_policy_tests.cpp
	gs_framebuffer_fetch_policy_tests.cpp
	gs_gpu_driver_profile_tests.cpp
	gs_draw_staging_tests.cpp
	gs_draw_log_tests.cpp
	gs_snapshot_policy_tests.cpp
	gs_sw_scanline_cpath_tests.cpp
	gs_tile_selection_policy_tests.cpp
	gs_page_bitmap_tests.cpp
	gs_vram_model_tests.cpp
	gs_tile_target_pool_tests.cpp
	gs_tile_lowering_tests.cpp
	gs_tile_oracle_tests.cpp
	gs_sw_scanline_date_tests.cpp
	gs_sw_scanline_dither_tests.cpp
	gs_sw_scanline_texfunc_tests.cpp
	gs_sw_scanline_tclag_tests.cpp
	gs_sw_depth_walk_tests.cpp
	gs_local_memory_move_tests.cpp
	gs_clut_tests.cpp
	gs_tile_zwalk_tests.cpp
	gs_tile_cwalk_tests.cpp
	gs_tile_twalk_tests.cpp
	gs_tile_swizzle_forms_tests.cpp
	gs_tile_clut_mirror_tests.cpp
	gs_tilegpu_texel_address_tests.cpp
	gs_tile_source_pin_tests.cpp
	gs_tile_cache_index_tests.cpp
	gs_tilegpu_materialise_tests.cpp
	gs_tilegpu_source_road_tests.cpp
	gs_tilegpu_donor_road_tests.cpp
	gs_tilegpu_clut_gather_tests.cpp
	gs_tilegpu_fbmsk_mask_tests.cpp
	gs_tilegpu_blend_tests.cpp
	gs_tilegpu_alpha_test_fold_tests.cpp
	gs_tilegpu_pass_key_tests.cpp
	gs_tilegpu_pass_geometry_tests.cpp
	gs_tilegpu_readback_store_tests.cpp
	gs_tilegpu_pull_grouping_tests.cpp
	gs_tilegpu_ring_compose_tests.cpp
	gs_tilegpu_ct16_road_tests.cpp
	gs_tilegpu_depth_seed_tests.cpp
	gs_tilegpu_texture_shuffle_tests.cpp
	gs_tilegpu_shader_budget_tests.cpp
	gs_fe_decode_tests.cpp
)

# The TileGpu fragment size gate compiles the real shader source, so it needs to find it. The
# runtime tree under build/bin/resources is a symlink to exactly this directory, but the test binary
# is not launched from there and a gate should not depend on a working directory. The depth seed's
# suite reads three shaders for the same reason: it holds the seed's guest-word -> depth map to the
# draw road's own, and a gate on a constant has to read the constant.
set_source_files_properties(gs_tilegpu_shader_budget_tests.cpp gs_tilegpu_depth_seed_tests.cpp PROPERTIES
	COMPILE_DEFINITIONS ARMSX2_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/bin/resources/shaders")

target_include_directories(gs_vertex_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_link_libraries(gs_vertex_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)
