# librashader — RetroArch (.slangp) shader chains for the GS post-process path.
#
# librashader is a RUST project, so it can't be vendored the way the C/C++ 3rdparty deps
# are: this repo keeps no git submodules and no checked-in binaries. Instead we fetch a
# PINNED commit at configure time and cargo-build its C-ABI library, exactly as the
# Android build does. Nothing lands in git, and the pin keeps the build reproducible.
#
# RUST IS OPTIONAL. No cargo means no target and ARMSX2_HAVE_LIBRASHADER stays off, so the
# shader chain compiles out via #ifdef instead of breaking a build for anyone without a
# Rust toolchain.
#
# Only runtime-metal is built: iOS has no OpenGL and no Vulkan renderer.

set(LIBRASHADER_PIN "87e8a97b50516d997defeaa168173dcd185d4022"
    CACHE STRING "Pinned librashader commit. Bump deliberately — never track a branch.")

# ---- Rust toolchain (optional) ---------------------------------------------------
find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{CARGO_HOME}/bin")
if(NOT CARGO_EXECUTABLE)
	message(STATUS "librashader: cargo not found -> RetroArch shader support DISABLED. "
	               "Install Rust (https://rustup.rs) to enable it.")
	set(ARMSX2_HAVE_LIBRASHADER OFF PARENT_SCOPE)
	return()
endif()

find_package(Git QUIET)
if(NOT Git_FOUND)
	message(STATUS "librashader: git not found -> RetroArch shader support DISABLED.")
	set(ARMSX2_HAVE_LIBRASHADER OFF PARENT_SCOPE)
	return()
endif()

# ---- Platform gate ----------------------------------------------------------------
# Only the device SDK maps onto a Rust target we build. A simulator or Catalyst configure
# must keep working, so it disables the feature rather than failing.
if(ARMSX2_MAC_CATALYST OR NOT CMAKE_OSX_SYSROOT STREQUAL "iphoneos")
	message(STATUS "librashader: sysroot '${CMAKE_OSX_SYSROOT}' is not iphoneos -> DISABLED.")
	set(ARMSX2_HAVE_LIBRASHADER OFF PARENT_SCOPE)
	return()
endif()
set(_lrs_rust_target "aarch64-apple-ios")

# ---- Fetch the pinned source (configure time, nothing vendored) --------------------
# librashader has no root CMakeLists, so point SOURCE_SUBDIR at a path that doesn't
# exist: that populates the tree WITHOUT trying to add_subdirectory() it.
#
# The patch fixes an upstream cfg gate that reads target_os = "macos" where every sibling
# gate reads target_vendor = "apple", so iOS fails to compile with E0425 on
# FilterChainMetal. Upstream master is STILL broken, so bumping the pin does not remove
# the need for this — do not delete it on the assumption someone fixed it.
# Reverse-check-then-apply keeps re-configure idempotent: git apply fails on an
# already-patched tree, and FetchContent re-runs PATCH_COMMAND more often than expected.
get_filename_component(_lrs_ios_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(_lrs_patch "${_lrs_ios_root}/patches/librashader-ctypes-apple-vendor.patch")

include(FetchContent)
FetchContent_Declare(librashader_src
	GIT_REPOSITORY https://github.com/SnowflakePowered/librashader.git
	GIT_TAG        ${LIBRASHADER_PIN}
	SOURCE_SUBDIR  cmake-does-not-exist
	PATCH_COMMAND  sh -c "'${GIT_EXECUTABLE}' apply --reverse --check '${_lrs_patch}' || '${GIT_EXECUTABLE}' apply '${_lrs_patch}'"
)
FetchContent_MakeAvailable(librashader_src)

set(_lrs_out_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(_lrs_lib "${_lrs_out_dir}/${_lrs_rust_target}/release/liblibrashader_capi.a")

# ---- cargo build ------------------------------------------------------------------
# STATIC, settled 2026-08-16 on measurement: against an embedded dylib it costs 9.91 MiB
# of executable but only 3,836 bytes of IPA, so size decided nothing and the shape that
# adds no embedded binary to a sideloaded bundle won. Full table in the phase's
# 04.3-LINK-MODE.md. Android's cdylib reasoning was checked and does not transfer here.
#
# IPHONEOS_DEPLOYMENT_TARGET is read from the cache variable rather than written literally
# because rustc otherwise links at iOS 10.0 while cc builds librashader's C++ deps against
# the current SDK, and the link dies on ___chkstk_darwin. Two values that can drift apart
# reintroduce that silently.
add_custom_command(
	OUTPUT "${_lrs_lib}"
	COMMAND ${CMAKE_COMMAND} -E env
		"IPHONEOS_DEPLOYMENT_TARGET=${ARMSX2_IOS_DEPLOYMENT_TARGET}"
		${CARGO_EXECUTABLE} build --release
			--manifest-path "${librashader_src_SOURCE_DIR}/librashader-capi/Cargo.toml"
			--target ${_lrs_rust_target}
			--target-dir "${_lrs_out_dir}"
			--no-default-features
			--features runtime-metal
	WORKING_DIRECTORY "${librashader_src_SOURCE_DIR}"
	COMMENT "librashader: cargo build (${_lrs_rust_target}, metal)"
	VERBATIM
)
add_custom_target(librashader_cargo DEPENDS "${_lrs_lib}")

add_library(librashader STATIC IMPORTED GLOBAL)
set_target_properties(librashader PROPERTIES IMPORTED_LOCATION "${_lrs_lib}")
target_include_directories(librashader INTERFACE "${librashader_src_SOURCE_DIR}/include")
add_dependencies(librashader librashader_cargo)

set(ARMSX2_HAVE_LIBRASHADER ON PARENT_SCOPE)
message(STATUS "librashader: ENABLED (${_lrs_rust_target}, metal, static)")
