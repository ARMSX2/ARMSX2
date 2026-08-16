# LSFG — Lossless Scaling frame generation, via lsfg-vk-android.
#
# Fetched at configure time from a pinned commit rather than vendored, for the same reason
# librashader is: the source tree plus its four submodules is ~130 MB, most of it test data
# and a Rust UI we never build.
#
# WHAT WE TAKE AND WHAT WE DO NOT
#
# Only the MIT-licensed `lsfg-vk-android` repository, and inside it only the framegen static
# library. Its sibling `LSFG-Android-Application` carries a "No Play Store, No Commercial Use"
# licence and is deliberately NOT fetched — nor is it needed. That app exists to capture the
# screen with MediaProjection and composite over another process, because Android 12+ forbids
# injecting code into a non-debuggable app. ARMSX2 owns its own swapchain, so it drives the
# library's AHardwareBuffer entry points directly: no screen capture, no overlay window, no
# accessibility service.
#
# The `liblsfg-vk.so` Vulkan-layer target from upstream is also skipped. It fails to link
# against the NDK anyway (vkGetAndroidHardwareBufferPropertiesANDROID is an extension entry
# point, not a plain export, so it must come from vkGetDeviceProcAddr) — and a layer is the
# wrong shape for an in-process caller. We link the static framegen lib and resolve Vulkan
# through the loader the GS backend already has.
#
# NOTHING PROPRIETARY IS FETCHED OR SHIPPED. The frame-generation shaders are not in this
# repository or upstream's; they are read at runtime out of the user's own legitimately
# purchased Lossless.dll, which they supply through the Storage Access Framework. This build
# only produces the code that can read one.
#
# OPTIONAL, like librashader and the Discord SDK: ARMSX2_ENABLE_LSFG defaults OFF and the play
# flavour never sets it, so the feature compiles out entirely rather than failing to configure.

if(NOT ARMSX2_ENABLE_LSFG)
	message(STATUS "LSFG: disabled (ARMSX2_ENABLE_LSFG is off) — frame generation compiles out")
	set(ARMSX2_HAVE_LSFG OFF PARENT_SCOPE)
	return()
endif()

if(NOT ANDROID)
	message(STATUS "LSFG: skipped — the AHardwareBuffer path is Android-only")
	set(ARMSX2_HAVE_LSFG OFF PARENT_SCOPE)
	return()
endif()

# Pinned to a COMMIT, which is what the paragraph below actually asks for.
#
# This said `release` — a branch — while the comment claimed it was pinned, so every configure
# re-resolved it and a remote push could silently change what the core links. That is the exact
# failure the comment was written to prevent, and it went unnoticed because a branch name reads
# like a tag at a glance. This library reaches into Vulkan device internals and ships inside our
# APK; it does not get to move on its own.
#
# 3e89e54 "feat: enable shaderFloat16 support in logical device creation" is the tip of `release`
# that ARMSX2 has actually been built and verified against. Bump it deliberately, and re-verify.
set(LSFG_PIN "3e89e5439a98f55d5acb003d20039426ab24e69c"
	CACHE STRING "lsfg-vk-android commit to build against")

include(FetchContent)
FetchContent_Declare(lsfg_src
	GIT_REPOSITORY https://github.com/FrankBarretta/lsfg-vk-android.git
	GIT_TAG        ${LSFG_PIN}
	# GIT_SHALLOW cannot fetch an arbitrary commit — a shallow clone only carries branch tips,
	# so it works for a ref name and fails for the SHA above.
	GIT_SHALLOW    FALSE
	GIT_SUBMODULES_RECURSE TRUE
	# Populate only; its top-level CMakeLists builds the layer .so we do not want, and adding
	# it wholesale would drag the Rust UI target in with it.
	SOURCE_SUBDIR  framegen
)
FetchContent_MakeAvailable(lsfg_src)

if(NOT EXISTS "${lsfg_src_SOURCE_DIR}/framegen/CMakeLists.txt")
	message(WARNING "LSFG: fetch produced no framegen/ — compiling the feature out")
	set(ARMSX2_HAVE_LSFG OFF PARENT_SCOPE)
	return()
endif()

# --- the shader path -------------------------------------------------------------------
# framegen does not read Lossless.dll. Its initialize() takes a loader callback and expects
# SPIR-V back, so the caller owns the whole chain: pull the RCDATA resources out of the PE,
# then translate each one from DXBC to SPIR-V. Upstream does that in its layer .so, which we
# do not build — so we assemble the same two pieces here and drive them from GSLsfg.cpp.
#
#   pe-parse  reads the resource directory out of the user's DLL
#   dxbc      DXVK's DXBC->SPIR-V compiler, which does the actual translation
#   trans.cpp upstream's thin wrapper over dxbc that also rewrites descriptor bindings
#
# extract.cpp is deliberately NOT taken: it hunts for Steam install paths and pulls in
# upstream's toml11 config system, neither of which means anything on Android. Our path
# comes from the SAF picker, so GSLsfg.cpp does that ~30 lines of pe-parse itself.
foreach(dep dxbc pe-parse/pe-parser-library volk)
	if(NOT EXISTS "${lsfg_src_SOURCE_DIR}/thirdparty/${dep}/CMakeLists.txt")
		message(WARNING "LSFG: thirdparty/${dep} missing (submodules not fetched?) — compiling out")
		set(ARMSX2_HAVE_LSFG OFF PARENT_SCOPE)
		return()
	endif()
endforeach()
if(NOT TARGET lsfg-vk-framegen)
	message(WARNING "LSFG: lsfg-vk-framegen target absent after fetch — compiling the feature out")
	set(ARMSX2_HAVE_LSFG OFF PARENT_SCOPE)
	return()
endif()

add_subdirectory("${lsfg_src_SOURCE_DIR}/thirdparty/dxbc"
	"${CMAKE_CURRENT_BINARY_DIR}/dxbc" EXCLUDE_FROM_ALL)
add_subdirectory("${lsfg_src_SOURCE_DIR}/thirdparty/pe-parse/pe-parser-library"
	"${CMAKE_CURRENT_BINARY_DIR}/pe-parse" EXCLUDE_FROM_ALL)
# framegen links volk PUBLIC. It is NOT in the fetch's SOURCE_SUBDIR, so it has to be added
# by hand or the framegen compile fails on a missing <volk.h>.
add_subdirectory("${lsfg_src_SOURCE_DIR}/thirdparty/volk"
	"${CMAKE_CURRENT_BINARY_DIR}/volk" EXCLUDE_FROM_ALL)

# Upstream's translator, compiled on its own so we inherit their binding-rewrite fixes
# instead of forking a copy of it into our tree.
add_library(lsfg-shader-translate STATIC "${lsfg_src_SOURCE_DIR}/src/extract/trans.cpp")
set_target_properties(lsfg-shader-translate PROPERTIES
	CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(lsfg-shader-translate PUBLIC "${lsfg_src_SOURCE_DIR}/include")
# trans.cpp includes <thirdparty/spirv.hpp>, which lives under dxbc's include/spirv.
target_link_libraries(lsfg-shader-translate PUBLIC dxbc)

# VK_USE_PLATFORM_ANDROID_KHR is REQUIRED, not cosmetic: without it the Vulkan headers hide
# VkAndroidHardwareBufferFormatPropertiesANDROID and friends, and framegen/src/core/image.cpp
# fails with three "unknown type name" errors. Upstream's own Gradle defines it, which is why
# a bare CMake configure of that repo does not build.
#
# PRIVATE, not PUBLIC: framegen's own sources need it, but its public header does not, and
# PUBLIC leaks -DVK_USE_PLATFORM_ANDROID_KHR onto every consumer — where it collides with
# VKLoader.h's own valueless #define and emits a macro-redefined warning in each of the
# hundreds of GS translation units.
target_compile_definitions(lsfg-vk-framegen PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
# volk needs it too, and for a reason that is not obvious: volk generates its PFN_* globals
# from the same platform macros, so without this it never defines
# vkGetAndroidHardwareBufferPropertiesANDROID while framegen — compiled WITH the macro —
# references it. The result is a single undefined symbol at link, pointing at framegen rather
# than at the volk target that actually dropped it.
target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
# The interpolation runs on its own VkDevice, so it needs the AHB import/export entry points
# from libvulkan plus libandroid for AHardwareBuffer itself.
target_link_libraries(lsfg-vk-framegen PUBLIC vulkan android)

# ARMSX2's own build turns exceptions OFF globally (BuildParameters.cmake, add_compile_options
# at top level, inherited by every subdirectory added after it). Every one of these libraries
# reports failure by throwing — 62 throw sites in framegen, 53 in dxbc — so they do not merely
# warn without this, they fail to compile. Re-enabled per target rather than by editing the
# global flag, so nothing else in the emulator gains exception handling as a side effect.
foreach(lsfg_target lsfg-vk-framegen dxbc lsfg-shader-translate)
	target_compile_options(${lsfg_target} PRIVATE -fexceptions)
endforeach()

# --- the isolation boundary -------------------------------------------------------------
#
# framegen goes into its OWN shared object, never into libemucore. volk (which framegen links)
# defines 759 globals named vkCreateImage, vkQueueSubmit and so on — precisely the names
# PCSX2's VKLoader.cpp defines. In one library that is a duplicate-symbol error at best; at
# worst the linker merges them and framegen's volkLoadDevice() call, made against ITS OWN
# VkDevice, silently repoints every entry point the GS renderer uses. A separate .so gives
# volk its own copies and is the only arrangement where the two loaders can coexist.
#
# The interface is C, for the second half of the same problem: ANDROID_STL=c++_static means
# each .so carries its own libc++, so an std::vector crossing the boundary would be two
# unrelated types sharing a name. See armsx2_lsfg_shim.h.
add_library(armsx2_lsfg SHARED "${CMAKE_CURRENT_SOURCE_DIR}/armsx2_lsfg_shim.cpp")
set_target_properties(armsx2_lsfg PROPERTIES
	CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(armsx2_lsfg PRIVATE -fexceptions)
target_compile_definitions(armsx2_lsfg PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_link_libraries(armsx2_lsfg PRIVATE lsfg-vk-framegen)
# Everything is hidden except the handful of armsx2_lsfg_* entry points, which are marked
# visible in the shim itself. In particular framegen's own LSFG_3_1 symbols stay internal, so
# nothing can reach past the boundary by accident.
target_link_options(armsx2_lsfg PRIVATE -Wl,--exclude-libs,ALL)

set(ARMSX2_HAVE_LSFG ON PARENT_SCOPE)
# Only the shim header and the shader chain reach libemucore. framegen's headers deliberately
# do NOT: nothing in the core should be able to call it directly.
set(ARMSX2_LSFG_INCLUDE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${lsfg_src_SOURCE_DIR}/include"
	PARENT_SCOPE)
set(ARMSX2_LSFG_LIBRARIES lsfg-shader-translate pe-parse PARENT_SCOPE)
message(STATUS "LSFG: enabled — framegen isolated in libarmsx2_lsfg.so, core links the shader chain only")
