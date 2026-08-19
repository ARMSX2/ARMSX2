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
)

target_include_directories(gs_vertex_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_link_libraries(gs_vertex_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)
