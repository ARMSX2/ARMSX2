# ARMSX2 Android native build.
#
# This is a THIN entry point: it builds the single shared PCSX2 core that lives
# at the repository root (../../../../../../{common,pcsx2,3rdparty}) and layers
# the Android JNI shared library (emucore) plus Android-only dependencies
# (adrenotools) on top. The core is NO LONGER vendored under this directory --
# see REFACTOR_STATUS.md for the migration details.
#
# NOTE: This wiring is structurally correct but has NOT been compiled against a
# real Android NDK yet. Expect to iterate on include paths / 3rdparty sourcing
# (root 3rdparty vs the Android-vendored 3rdparty in this folder) via CI.

cmake_minimum_required(VERSION 3.16...3.25)

cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

project(ARMSX2Android C CXX)

# Absolute path to the repository root (the single source of truth for the core).
get_filename_component(ARMSX2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

# Use the Android-aware cmake modules vendored beside this entry point (copies of
# the known-good v7-clean modules: Pcsx2Utils has the ANDROID branch, BuildParameters
# cross-compiles to aarch64 + consumes the page-size arg, SearchForStuff sources the
# vendored 3rdparty instead of desktop find_package). The root cmake/ modules are
# desktop-only and FATAL on Android. Fold these back into root as a later unify pass.
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Android is a non-desktop frontend: no Qt UI.
set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)

# The core's common/ and pcsx2/ CMakeLists guard on this sentinel (it is normally
# set by the desktop top-level CMakeLists.txt, which the Android build bypasses).
set(TOP_CMAKE_WAS_SOURCED TRUE)

# The core's common/ and pcsx2/ CMakeLists guard on this sentinel (it is normally
# set by the desktop top-level CMakeLists.txt, which the Android build bypasses).
set(TOP_CMAKE_WAS_SOURCED TRUE)

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()

include(BuildParameters)
include(SearchForStuff)

get_git_version_info()
write_svnrev_h()

# Build the shared core from the repository root. Each add_subdirectory needs an
# explicit binary dir because the source dirs live outside this tree.
# NOTE: the individual 3rdparty libs are pulled in by SearchForStuff (above),
# relative to this cpp dir's vendored 3rdparty/. Root 3rdparty/ has no aggregate
# CMakeLists, so add_subdirectory'ing it would FATAL; do not re-add it here.
add_subdirectory("${ARMSX2_ROOT}/common"   "${CMAKE_BINARY_DIR}/common")
# Header-only libretro API definitions (INTERFACE target `libretro-headers`,
# not vendored under this dir's 3rdparty/): pcsx2's Vulkan GS source list
# includes VKLibretro.cpp unconditionally (runtime-gated outside the libretro
# frontend), and it needs libretro_vulkan.h. Must exist before pcsx2 below —
# CMake silently treats an unknown target name in target_link_libraries as a
# raw linker flag, so a missing definition surfaces only as a compile error.
add_subdirectory("${ARMSX2_ROOT}/3rdparty/libretro" "${CMAKE_BINARY_DIR}/3rdparty/libretro")
add_subdirectory("${ARMSX2_ROOT}/pcsx2"    "${CMAKE_BINARY_DIR}/pcsx2")

# Android-only 3rdparty: libadrenotools (custom Vulkan ICD loading for Adreno).
# Provides the static `adrenotools` lib linked into emucore plus runtime hook
# .so's shipped in the apk's lib/<abi>/.

# ---------------------------------------------------------------------------
# Flurry -> libflurry.so
#
# Calum Robinson's Flurry screensaver (2002), offered as a live library background. Its own
# target rather than folded into the core for two reasons: it is BSD-3-clause next to GPL code
# and that boundary should be visible, and it is plain C from 2002 that wants none of the C++20
# the emulator is built with.
#
# gl_compat.c answers the GL 1.x calls the sources make — client-side vertex arrays, a
# fixed-function ortho and GL_QUADS — with a GLES2 shader, so the renderer itself stays
# unmodified. See its header for what is and is not emulated.
#
# Ported from ARMSX3, where it already ships. Unconditional: it is ~100 KB of C with no
# dependency beyond GLES2, and the feature is off by default at the UI level.
# ---------------------------------------------------------------------------
add_library(flurry SHARED
    flurry/flurry_jni.c
    flurry/gl_compat.c
    flurry/flurry.c
    flurry/flurry-smoke.c
    flurry/flurry-spark.c
    flurry/flurry-star.c
    flurry/flurry-texture.c
)

target_include_directories(flurry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/flurry)
set_target_properties(flurry PROPERTIES C_STANDARD 99)

# -ffast-math: a particle simulation judged by eye, running thousands of sqrt per frame.
# Upstream shipped hand-written frsqrte paths for exactly this reason.
target_compile_options(flurry PRIVATE -O2 -ffast-math -Wall -Wno-unused-parameter)
target_link_libraries(flurry log GLESv2 m)


# ---------------------------------------------------------------------------
# Really Slick Screensavers -> libsavers.so
#
# Terry Welsh's savers (GPL-2.0-or-later), compiled against a GLES2 shim rather than rewritten.
# The saver sources are BYTE-IDENTICAL to upstream: they are built with RS_XSCREENSAVER, which
# selects their platform-neutral path, and everything that path expects is answered by
# compat/. That keeps them re-pullable and keeps them recognisably his code.
#
# Separate from the flurry target because these are C++ and Flurry is C99, and because Flurry's
# shim is a different, smaller one. If a saver ever needs Flurry's, the two can merge.

add_library(savers SHARED
    savers/savers_jni.cpp
    savers/savers_platform.cpp
    savers/gl1.c

    # Each saver is compiled through a *_unit.cpp that wraps it in a namespace: they all
    # declare the same globals (draw, idleProc, cleanUp, readyToDraw) because each was built
    # as its own executable, and two in one .so collide at link time.
    savers/flux/flux_unit.cpp
    savers/flux/flux_port.cpp
    savers/plasma/plasma_unit.cpp
    savers/plasma/plasma_port.cpp
    savers/solarwinds/solarwinds_unit.cpp
    savers/solarwinds/solarwinds_port.cpp

    # Hyperspace. extensions.cpp is deliberately NOT built: it is the WIN32/GLX loader for the
    # ARB shader path, and the port takes the non-shader path instead. The entry points it
    # would have resolved are declared in compat/arb_shaders.h and defined in savers_platform.
    savers/hyperspace/unit_causticTextures.cpp
    savers/hyperspace/unit_flare.cpp
    savers/hyperspace/unit_goo.cpp
    savers/hyperspace/unit_hyperspace.cpp
    savers/hyperspace/unit_splinePath.cpp
    savers/hyperspace/unit_starBurst.cpp
    savers/hyperspace/unit_stretchedParticle.cpp
    savers/hyperspace/unit_tunnel.cpp
    savers/hyperspace/unit_wavyNormalCubeMaps.cpp
    savers/hyperspace/hyperspace_port.cpp

    savers/lattice/lattice_unit.cpp
    savers/lattice/lattice_port.cpp

    # Skyrocket. soundEngine.cpp is NOT built and its ~7MB of samples are not shipped: upstream
    # drives OpenAL, the port runs with dSound = 0, and soundEngine.h is a stub.
    savers/skyrocket/unit_flare.cpp
    savers/skyrocket/unit_particle.cpp
    savers/skyrocket/unit_shockwave.cpp
    savers/skyrocket/unit_skyrocket.cpp
    savers/skyrocket/unit_smoke.cpp
    savers/skyrocket/unit_world.cpp
    savers/skyrocket/skyrocket_port.cpp

    savers/rslibs/rsMath/rsVec.cpp
    savers/rslibs/rsMath/rsVec4.cpp
    savers/rslibs/rsMath/rsMatrix.cpp
    savers/rslibs/rsMath/rsQuat.cpp
    savers/rslibs/Rgbhsl/Rgbhsl.cpp

    # Implicit surfaces: Hyperspace's "goo" is a marching-cubes isosurface.
    savers/rslibs/Implicit/impSurface.cpp
    savers/rslibs/Implicit/impCubeVolume.cpp
    savers/rslibs/Implicit/impCubeTables.cpp
    savers/rslibs/Implicit/impShape.cpp
    savers/rslibs/Implicit/impSphere.cpp
    savers/rslibs/Implicit/impCapsule.cpp
    savers/rslibs/Implicit/impEllipsoid.cpp
    savers/rslibs/Implicit/impHexahedron.cpp
    savers/rslibs/Implicit/impRoundedHexahedron.cpp
    savers/rslibs/Implicit/impKnot.cpp
    savers/rslibs/Implicit/impTorus.cpp

)

target_include_directories(savers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/savers
    ${CMAKE_CURRENT_SOURCE_DIR}/savers/compat
    ${CMAKE_CURRENT_SOURCE_DIR}/savers/rslibs
    ${CMAKE_CURRENT_SOURCE_DIR}/savers/flux
    ${CMAKE_CURRENT_SOURCE_DIR}/savers/plasma
    ${CMAKE_CURRENT_SOURCE_DIR}/savers/solarwinds
    ${CMAKE_CURRENT_SOURCE_DIR}/savers/hyperspace
    ${CMAKE_CURRENT_SOURCE_DIR}/savers/lattice
    ${CMAKE_CURRENT_SOURCE_DIR}/savers/skyrocket
)

# RS_XSCREENSAVER picks the savers' platform-neutral path. Without it they compile their Win32
# shell and nothing links.
target_compile_definitions(savers PRIVATE RS_XSCREENSAVER)
set_target_properties(savers PROPERTIES C_STANDARD 99 CXX_STANDARD 17)

# -ffast-math for the same reason as Flurry: particle simulation judged entirely by eye.
# -Wno-missing-braces: Welsh's code initialises nested aggregates without inner braces
# throughout, which -Wall diagnoses once per site per TU -- ~5 MILLION lines and an 842 MB
# build log, which is how it was found. These sources are upstream and are not ours to
# restyle, so the diagnostic has nothing to tell us. Purely a diagnostic flag: no codegen
# difference.
target_compile_options(savers PRIVATE -O2 -ffast-math -Wall -Wno-unused-parameter -Wno-missing-braces)
target_link_libraries(savers log GLESv2 m)



add_subdirectory(3rdparty/adrenotools)

# pcsx2-gsrunner as an on-demand NDK executable (adb-shell oracle/CI runs on
# Android devices — the gs_oracle.py device protocol). EXCLUDE_FROM_ALL keeps
# every APK build unchanged; build it explicitly with --target pcsx2-gsrunner.
add_subdirectory("${ARMSX2_ROOT}/pcsx2-gsrunner" "${CMAKE_BINARY_DIR}/pcsx2-gsrunner" EXCLUDE_FROM_ALL)

# librashader: RetroArch (.slangp) shader chains for the GS post-process path, for BOTH
# the Vulkan and OpenGL(ES) renderers. Rust-built and fetched at configure time from a
# pinned commit (see the subdir's CMakeLists for why it can't be vendored). OPTIONAL:
# with no cargo installed this leaves ARMSX2_HAVE_LIBRASHADER off and the feature simply
# compiles out, so a missing Rust toolchain can never break the build.
add_subdirectory(3rdparty/librashader)

# LSFG: Lossless Scaling frame generation, github flavour only — ARMSX2_ENABLE_LSFG defaults
# OFF and only the github flavour turns it on, so a play build compiles the feature out.
#
# ★ There is no 3rdparty/lsfg subdirectory any more. The implementation was replaced by the
# port of Eden's (eden-emu PR #4263), which runs as ordinary compute on the emulator's own
# device and needs NONE of what the old one dragged in: no lsfg-vk-android framegen library,
# no DXVK dxbc compiler, no pe-parse, no volk, and so no separate .so to dlopen. The whole
# subtree and its ~130 MB fetch are gone; the ported sources live in
# pcsx2/GS/Renderers/Vulkan/FrameGen and build straight into the core.

# Discord Social SDK (rich presence + friends). Staged by hand rather than consumed as the
# vendor .aar: that .aar's manifest declares RECORD_AUDIO and four foreground-service
# permissions for its voice features, and the manifest merger would fold all of them into
# ARMSX2. See app/build.gradle.kts for the rest of that reasoning.
#
# OPTIONAL, exactly like librashader above: a tree without the .so staged simply compiles the
# feature out rather than failing to configure, so nobody needs a 680 MB SDK download to build.
# Discord Social SDK — an OPTIONAL, PRIVATE build input.
#
# Discord's terms let us ship the SDK inside a working application, but not republish the raw
# binary on its own. So it lives outside this repository and is supplied at build time through
# DISCORD_SDK_DIR, which private release CI sets:
#
#   $DISCORD_SDK_DIR/include/discordpp.h
#   $DISCORD_SDK_DIR/include/cdiscord.h
#   $DISCORD_SDK_DIR/<abi>/libdiscord_partner_sdk.so
#   $DISCORD_SDK_DIR/discord_partner_sdk.aar     (consumed by build.gradle.kts)
#
# Unset — which is the case for every public clone — the whole feature compiles out: no helper
# library, no SDK, and the UI reports itself unavailable. That is a supported configuration, not a
# broken build, and it is deliberately NOT a product flavour: one release variant, one code path,
# and the only difference is whether a private directory was present.
if(NOT DISCORD_SDK_DIR AND DEFINED ENV{DISCORD_SDK_DIR})
	set(DISCORD_SDK_DIR "$ENV{DISCORD_SDK_DIR}")
endif()

set(DISCORD_SDK_SO "${DISCORD_SDK_DIR}/${ANDROID_ABI}/libdiscord_partner_sdk.so")
set(DISCORD_SDK_INCLUDE "${DISCORD_SDK_DIR}/include")

if(DISCORD_SDK_DIR AND EXISTS "${DISCORD_SDK_SO}" AND EXISTS "${DISCORD_SDK_INCLUDE}/discordpp.h")
	add_library(discord_partner_sdk SHARED IMPORTED)
	set_target_properties(discord_partner_sdk PROPERTIES
		IMPORTED_LOCATION "${DISCORD_SDK_SO}"
		INTERFACE_INCLUDE_DIRECTORIES "${DISCORD_SDK_INCLUDE}")
	set(ARMSX2_HAVE_DISCORD ON)
	message(STATUS "Discord Social SDK: ${DISCORD_SDK_SO}")
else()
	set(ARMSX2_HAVE_DISCORD OFF)
	message(STATUS "Discord Social SDK not supplied (DISCORD_SDK_DIR unset) — presence/friends compiled out")
endif()

set(ARMSX2_EMUCORE_LIBRARY_NAME "emucore_4k" CACHE STRING "Android emucore shared library target/output name")

add_library(${ARMSX2_EMUCORE_LIBRARY_NAME} SHARED
	native-lib.cpp
)

# The Discord bridge is its OWN shared library, and emucore must never link it.
#
# ARMSX2 is GPL-3.0+; the Discord Social SDK is proprietary with no published licence. Linking the
# two into one binary would make the emulator a combined work with a library we cannot supply
# corresponding source for. Keeping them as separate programs that talk over IPC is what makes the
# feature shippable at all, and the linker is where that separation is actually enforced:
#
#   libarmsx2_discord.so  -> discord_partner_sdk   (loaded ONLY in the :discord process)
#   libemucore_*.so       -> no Discord anything
#
# discord_bridge.cpp includes no PCSX2 header, so this target needs none of PCSX2's include paths.
# If it ever fails to build for want of one, that is the signal that something GPL crept in — fix
# the include, do not add the path.
#
# Verify after any build:  llvm-readelf -d libemucore_4k.so | grep -i discord   (must be empty)
if(ARMSX2_HAVE_DISCORD)
	add_library(armsx2_discord SHARED discord_bridge.cpp)
	target_link_libraries(armsx2_discord PRIVATE discord_partner_sdk log)
	target_compile_definitions(armsx2_discord PRIVATE ARMSX2_HAS_DISCORD=1)
	set_target_properties(armsx2_discord PROPERTIES OUTPUT_NAME "armsx2_discord")
endif()

target_link_libraries(${ARMSX2_EMUCORE_LIBRARY_NAME} PRIVATE
	PCSX2
	PCSX2_FLAGS
	common
	imgui
	libzip::zip
	SDL3::SDL3
	shaderc
	vixl
	adrenotools
	android
	log
)

# ★ AFTER the emucore target exists — add_dependencies needs a target that has already been
# created, and stating this next to the flurry target itself (which is defined earlier in this
# file) fails configure outright with "Cannot add target-level dependencies to non-existent
# target".
#
# libflurry.so is loaded by System.loadLibrary from Kotlin and never linked, so nothing else in
# this build would make it. A missing .so stays invisible until someone switches the background
# on and the view throws UnsatisfiedLinkError.
add_dependencies(${ARMSX2_EMUCORE_LIBRARY_NAME} flurry)
add_dependencies(${ARMSX2_EMUCORE_LIBRARY_NAME} savers)

target_include_directories(${ARMSX2_EMUCORE_LIBRARY_NAME} PRIVATE
	"${ARMSX2_ROOT}"
	"${ARMSX2_ROOT}/pcsx2"
	# IconsFontAwesome.h, for the Discord bridge's OSD notification. Core sources include it bare
	# because it is on the PCSX2 target's path; emucore has its own, so state it rather than rely
	# on that propagating.
	"${ARMSX2_ROOT}/3rdparty/include"
	"${CMAKE_CURRENT_SOURCE_DIR}"
)

# Only link librashader when the Rust toolchain actually produced it; a cargo-less build
# leaves the shader chain #ifdef'd out instead of failing to link.
#
# The define MUST go on PCSX2_FLAGS, not PCSX2. The shader chain lives in
# GS/Renderers/*/GSDevice*.cpp, and release builds pass -DLTO_PCSX2_CORE=ON, which compiles
# those sources into a *separate* PCSX2_LTO target (see pcsx2/CMakeLists.txt); PRIVATE
# properties on PCSX2 never reach them. PCSX2_FLAGS is the INTERFACE target both PCSX2 and
# PCSX2_LTO link, so it is the only place the define (and the header's include dir) reaches
# every core translation unit under either LTO setting.
#
# Getting this wrong costs a build to notice: the chain is behind #ifdef ARMSX2_HAS_LIBRASHADER,
# so a missed define compiles cleanly, links cleanly, and silently ships the feature stubbed
# out. Verify with `strings` on the built core, never the build log.
if(ARMSX2_HAVE_LIBRASHADER)
	target_link_libraries(PCSX2_FLAGS INTERFACE librashader)
	target_compile_definitions(PCSX2_FLAGS INTERFACE ARMSX2_HAS_LIBRASHADER=1)
	target_link_libraries(${ARMSX2_EMUCORE_LIBRARY_NAME} PRIVATE librashader)
endif()

# Same PCSX2_FLAGS reasoning as librashader directly above, and for the same reason: the
# frame-generation hook lives in GS/Renderers/Vulkan/GSDeviceVK.cpp, which release builds
# compile into the separate PCSX2_LTO target. A define placed on PCSX2 would never reach it,
# the code behind #ifdef ARMSX2_HAS_LSFG would vanish, and the build would succeed anyway —
# shipping the toggle with nothing behind it. Verify with `strings` on the core, not the log.
if(ARMSX2_ENABLE_LSFG)
	# Only the define is needed now — the sources are part of the core, so there is nothing to
	# link and nothing to add_dependencies on. Gated on ARMSX2_ENABLE_LSFG rather than the old
	# ARMSX2_HAVE_LSFG, which was set by the subdirectory that no longer exists; leaving that
	# test in place would silently drop the define and ship the toggle with nothing behind it.
	target_compile_definitions(PCSX2_FLAGS INTERFACE ARMSX2_HAS_LSFG=1)
endif()
