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
	gs_snapshot_policy_tests.cpp
	external_display_aspect_tests.cpp
)

target_include_directories(gs_vertex_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_link_libraries(gs_vertex_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)
