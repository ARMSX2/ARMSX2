# Setting it to a range tells it that it supports the features on the newer
# versions as well, avoiding setting policies.
cmake_minimum_required(VERSION 3.16...3.25)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Enabling this cmake policy gets rid of warnings regarding LTO.
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
	message(FATAL_ERROR "PCSX2 does not support in-tree builds.  Please make a build directory that is not the PCSX2 source directory and generate your CMake project there using either `cmake -B build_directory` or by running cmake from the build directory.")
endif()

set(ARMSX2_VERSION_MAJOR 2)
set(ARMSX2_VERSION_MINOR 6)
set(ARMSX2_VERSION_PATCH 0)
set(ARMSX2_VERSION "${ARMSX2_VERSION_MAJOR}.${ARMSX2_VERSION_MINOR}.${ARMSX2_VERSION_PATCH}")
set(ARMSX2_BUILD_NUMBER 260)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
	set(CMAKE_C_COMPILER_WORKS 1)
	set(CMAKE_CXX_COMPILER_WORKS 1)

	option(ARMSX2_REAL_DEVICE "Build for real iOS device (iphoneos SDK)" OFF)
	option(ARMSX2_MAC_CATALYST "Build the ARMSX2 UIKit app for Mac Catalyst" OFF)
	if(ARMSX2_MAC_CATALYST)
		set(ARMSX2_DEFAULT_BUNDLE_IDENTIFIER "com.armsx2.macos")
	else()
		set(ARMSX2_DEFAULT_BUNDLE_IDENTIFIER "com.armsx2.ios")
	endif()
	set(ARMSX2_BUNDLE_IDENTIFIER "${ARMSX2_DEFAULT_BUNDLE_IDENTIFIER}" CACHE STRING "Apple bundle identifier")
	set(ARMSX2_DISPLAY_NAME "ARMSX2 iOS" CACHE STRING "Home screen name")
	set(ARMSX2_VERSION_SUFFIX "" CACHE STRING "Appended to CFBundleShortVersionString, e.g. -nightly.20260928.0812")
	set(ARMSX2_DEVELOPMENT_TEAM "" CACHE STRING "Apple Development Team ID for device signing")
	if(ARMSX2_REAL_DEVICE OR ARMSX2_MAC_CATALYST)
		set(ARMSX2_JIT_ENTITLEMENTS_DEFAULT ON)
	else()
		set(ARMSX2_JIT_ENTITLEMENTS_DEFAULT OFF)
	endif()
	option(ARMSX2_ENABLE_JIT_ENTITLEMENTS "Attach the Apple JIT entitlement file for provisioning profiles which allow it" ${ARMSX2_JIT_ENTITLEMENTS_DEFAULT})
	option(ARMSX2_IOS_DSYM "Emit a .dSYM for crash symbolication (injects -g into Release; costs link time + artifact size)" OFF)
	set(ARMSX2_IOS_DEPLOYMENT_TARGET "17.4" CACHE STRING "Minimum iOS deployment target")
	set(CMAKE_OSX_DEPLOYMENT_TARGET "${ARMSX2_IOS_DEPLOYMENT_TARGET}" CACHE STRING "Minimum iOS deployment target" FORCE)
	set(ARMSX2_MACOS_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum Mac Catalyst deployment target")
	option(ARMSX2_CORE_ONLY "Build only the core libraries (set by scripts/build-ios-core.sh)" OFF)
	if(ARMSX2_CORE_ONLY AND ARMSX2_MAC_CATALYST)
		set(CMAKE_OSX_SYSROOT "macosx")
	elseif(ARMSX2_REAL_DEVICE OR ARMSX2_MAC_CATALYST)
		set(CMAKE_OSX_SYSROOT "iphoneos")
	else()
		set(CMAKE_OSX_SYSROOT "iphonesimulator")
	endif()
	# Ninja turns CMAKE_OSX_SYSROOT into a path at project(); librashader picks its Rust target by name.
	set(ARMSX2_OSX_SDK "${CMAKE_OSX_SYSROOT}")
	set(CMAKE_OSX_ARCHITECTURES "arm64")
	set(USE_OPENGL OFF CACHE BOOL "Enable OpenGL renderer" FORCE)
	set(USE_VULKAN OFF CACHE BOOL "Enable Vulkan renderer" FORCE)
	set(SDL_OPENGL OFF CACHE BOOL "Enable SDL OpenGL support" FORCE)
	set(SDL_OPENGLES OFF CACHE BOOL "Enable SDL OpenGL ES support" FORCE)
	# iOS uses its own SwiftUI frontend, not the desktop Qt GUI or test harnesses.
	set(ENABLE_QT_UI OFF CACHE BOOL "Enables building the PCSX2 Qt interface." FORCE)
	set(ENABLE_TESTS OFF CACHE BOOL "Enables building the unit tests" FORCE)
	set(ENABLE_GSRUNNER OFF CACHE BOOL "Enables building the GSRunner" FORCE)
endif()

# Project Name
project(ARMSX2iOS VERSION ${ARMSX2_VERSION} LANGUAGES C CXX)

add_compile_definitions(ARMSX2_VERSION_STR="${ARMSX2_VERSION}")

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
	set(_M_ARM64 TRUE)
	set(_M_X86 FALSE)
	set(CMAKE_HOST_SYSTEM_PROCESSOR "arm64")
	set(CMAKE_SYSTEM_PROCESSOR "arm64")

	if(CMAKE_GENERATOR STREQUAL "Xcode")
		if(ARMSX2_MAC_CATALYST OR NOT ARMSX2_REAL_DEVICE)
			set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
			set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
			set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual")
			set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
			set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "")
		endif()
	endif()
endif()

# Variable to check that people use the good file
set(TOP_CMAKE_WAS_SOURCED TRUE)

# Absolute path to the repository root (single source of truth for the core).
# The PCSX2 core / common / 3rdparty are NO LONGER vendored under this iOS app;
# they are built from the repo root. See REFACTOR_STATUS.md.
get_filename_component(ARMSX2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# The iOS-aware modules vendored beside this entry point come first so they win
# over their root namesakes: BuildParameters sets Apple-Silicon ARM64 flags and
# SearchForStuff builds every dep from vendored sources rather than making the
# desktop find_package() calls iOS cannot satisfy. Root is the fallback, and
# Pcsx2Utils resolves there — it carries the IOS branch and the cross-compile
# page-size escape, and its detect_* functions run only under LINUX.
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" "${ARMSX2_ROOT}/cmake")

# include some generic functions to ensure correctness of the env
include(Pcsx2Utils)

check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()

#-------------------------------------------------------------------------------
# Include specific module
include(BuildParameters)

if(ARMSX2_CORE_ONLY)
	include(SearchForStuff)

	# Must be done after SearchForStuff
	get_git_version_info()
	write_svnrev_h()

	# Build the shared core from the repository root. Explicit binary dirs are
	# required because the source dirs live outside this iOS app tree.
	# NOTE: the individual 3rdparty libs are pulled in by SearchForStuff (above),
	# sourced from ${ARMSX2_ROOT}/3rdparty and the Android-vendored tree. The root
	# 3rdparty/ has no aggregate CMakeLists, so add_subdirectory'ing it would
	# FATAL; do not re-add it here.

	# make common
	add_subdirectory(${ARMSX2_ROOT}/common ${CMAKE_BINARY_DIR}/common)

	# make pcsx2
	add_subdirectory(${ARMSX2_ROOT}/pcsx2 ${CMAKE_BINARY_DIR}/pcsx2)
	# Fetched and cargo-built, never vendored. Must follow pcsx2 so PCSX2_FLAGS exists by the
	# time the link block below runs.
	block(PROPAGATE ARMSX2_HAVE_LIBRASHADER)
		set(CMAKE_OSX_SYSROOT "${ARMSX2_OSX_SDK}")
		add_subdirectory(3rdparty/librashader)
	endblock()

	# On PCSX2_FLAGS, not PCSX2, so every core target that links the interface sees it.
	if(ARMSX2_HAVE_LIBRASHADER)
		target_link_libraries(PCSX2_FLAGS INTERFACE librashader)
		target_compile_definitions(PCSX2_FLAGS INTERFACE ARMSX2_HAS_LIBRASHADER=1)
	endif()

	# write_svnrev_h only runs at configure; this keeps the core's banner on the commit being built.
	add_custom_target(armsx2_svnrev ALL
		BYPRODUCTS "${CMAKE_BINARY_DIR}/common/include/svnrev.h"
		COMMAND ${CMAKE_COMMAND}
			-DGIT_WORKING_DIR=${PROJECT_SOURCE_DIR}
			-DOUTPUT_FILE=${CMAKE_BINARY_DIR}/common/include/svnrev.h
			-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteSvnRev.cmake
		COMMENT "Resolving core git revision"
	)
	add_dependencies(PCSX2 armsx2_svnrev)

	add_custom_target(armsx2_ra_ua_stamp
		BYPRODUCTS "${CMAKE_BINARY_DIR}/ra_ua_present.txt"
		COMMAND ${CMAKE_COMMAND}
			-DHEADER=${ARMSX2_ROOT}/pcsx2/ra_ua_secret.h
			-DOUTPUT_FILE=${CMAKE_BINARY_DIR}/ra_ua_present.txt
			-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteRaUaStamp.cmake
	)
	set_source_files_properties(${ARMSX2_ROOT}/pcsx2/Host.cpp TARGET_DIRECTORY PCSX2
		PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/ra_ua_present.txt)
	add_dependencies(PCSX2 armsx2_ra_ua_stamp)

	add_custom_target(armsx2_core_libs)
	add_dependencies(armsx2_core_libs PCSX2 SDL3-static armsx2_svnrev armsx2_ra_ua_stamp)
	if(ARMSX2_HAVE_LIBRASHADER)
		add_dependencies(armsx2_core_libs librashader_cargo)
	endif()

	function(armsx2_collect_libraries directory out)
		set(armsx2_found "")
		get_property(armsx2_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
		foreach(armsx2_target IN LISTS armsx2_targets)
			get_target_property(armsx2_type ${armsx2_target} TYPE)
			if(armsx2_type MATCHES "^(STATIC|OBJECT|INTERFACE)_LIBRARY$")
				list(APPEND armsx2_found ${armsx2_target})
			endif()
		endforeach()

		get_property(armsx2_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
		foreach(armsx2_subdirectory IN LISTS armsx2_subdirectories)
			armsx2_collect_libraries("${armsx2_subdirectory}" armsx2_more)
			list(APPEND armsx2_found ${armsx2_more})
		endforeach()
		set(${out} ${armsx2_found} PARENT_SCOPE)
	endfunction()

	armsx2_collect_libraries("${CMAKE_SOURCE_DIR}" ARMSX2_CORE_LIBRARIES)
	# These export their own targets, and a target can sit in only one export set.
	list(REMOVE_ITEM ARMSX2_CORE_LIBRARIES SDL3_Headers SDL3-static SDL3_test plutovg plutosvg libzstd libzstd_static)
	export(TARGETS ${ARMSX2_CORE_LIBRARIES} NAMESPACE armsx2core:: FILE "${CMAKE_BINARY_DIR}/ARMSX2CoreTargets.cmake")
	set(ARMSX2_CORE_FOREIGN_EXPORTS
		"${CMAKE_BINARY_DIR}/3rdparty/sdl3/SDL3headersTargets.cmake"
		"${CMAKE_BINARY_DIR}/3rdparty/sdl3/SDL3staticTargets.cmake"
		"${CMAKE_BINARY_DIR}/3rdparty/plutovg/plutovgTargets.cmake"
		"${CMAKE_BINARY_DIR}/3rdparty/plutosvg/plutosvgTargets.cmake"
		"${CMAKE_BINARY_DIR}/3rdparty/zstd/build/cmake/zstdTargets_static.cmake"
	)

	if(ARMSX2_HAVE_LIBRASHADER)
		get_target_property(ARMSX2_LIBRASHADER_LIB librashader IMPORTED_LOCATION)
		get_target_property(ARMSX2_LIBRASHADER_INCLUDE librashader INTERFACE_INCLUDE_DIRECTORIES)
	endif()
	file(WRITE "${CMAKE_BINARY_DIR}/armsx2-core.cmake"
		"set(ARMSX2_HAVE_LIBRASHADER \"${ARMSX2_HAVE_LIBRASHADER}\")\n"
		"set(ARMSX2_LIBRASHADER_LIB \"${ARMSX2_LIBRASHADER_LIB}\")\n"
		"set(ARMSX2_LIBRASHADER_INCLUDE \"${ARMSX2_LIBRASHADER_INCLUDE}\")\n"
		"set(ARMSX2_ANDROID_VENDORED \"${ARMSX2_ANDROID_VENDORED}\")\n"
		"set(ARMSX2_CORE_FOREIGN_EXPORTS \"${ARMSX2_CORE_FOREIGN_EXPORTS}\")\n"
	)
	return()
endif()

find_program(ARMSX2_NINJA ninja)
if(NOT ARMSX2_NINJA)
	message(FATAL_ERROR "ninja is required to build the iOS core. Install it with: brew install ninja")
endif()
if(ARMSX2_MAC_CATALYST)
	set(ARMSX2_CORE_PLATFORM catalyst)
elseif(ARMSX2_REAL_DEVICE)
	set(ARMSX2_CORE_PLATFORM device)
else()
	set(ARMSX2_CORE_PLATFORM sim)
endif()
set(ARMSX2_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/core-${ARMSX2_CORE_PLATFORM}")
execute_process(
	COMMAND ${CMAKE_COMMAND} -E env ARMSX2_CMAKE=${CMAKE_COMMAND} ARMSX2_NINJA=${ARMSX2_NINJA}
		${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ios-core.sh ${ARMSX2_CORE_PLATFORM} --configure
	RESULT_VARIABLE armsx2_core_result
)
if(NOT armsx2_core_result EQUAL 0)
	message(FATAL_ERROR "configuring build/core-${ARMSX2_CORE_PLATFORM} failed")
endif()

# The exported core names these imported targets, so they have to exist here too.
find_package(Threads REQUIRED)
add_library(ZLIB::ZLIB INTERFACE IMPORTED)
set_property(TARGET ZLIB::ZLIB PROPERTY INTERFACE_LINK_LIBRARIES z)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DFMT_USE_EXCEPTIONS=0 -DFMT_USE_RTTI=0")
include("${ARMSX2_CORE_DIR}/armsx2-core.cmake")
if(ARMSX2_HAVE_LIBRASHADER)
	add_library(librashader STATIC IMPORTED)
	set_target_properties(librashader PROPERTIES
		IMPORTED_LOCATION "${ARMSX2_LIBRASHADER_LIB}"
		INTERFACE_INCLUDE_DIRECTORIES "${ARMSX2_LIBRASHADER_INCLUDE}")
endif()
# One Release core serves every app configuration.
foreach(armsx2_config DEBUG RELWITHDEBINFO MINSIZEREL)
	set(CMAKE_MAP_IMPORTED_CONFIG_${armsx2_config} Release)
endforeach()
foreach(armsx2_export IN LISTS ARMSX2_CORE_FOREIGN_EXPORTS)
	include("${armsx2_export}")
endforeach()
# SDL's headers were plain -I while SDL built in-tree; imported targets default to -isystem.
set_property(TARGET SDL3::Headers PROPERTY SYSTEM OFF)
include("${ARMSX2_CORE_DIR}/ARMSX2CoreTargets.cmake")
set(CMAKE_SUPPRESS_REGENERATION ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
	enable_language(Swift)

	# An empty glob would configure fine and ship an app without these files.
	function(armsx2_require_files var)
		if(NOT ${var})
			message(FATAL_ERROR "${var} is empty; its glob matched nothing")
		endif()
	endfunction()

	# Swift sources are globbed so newly-added files are picked up automatically at
	# configure time. CONFIGURE_DEPENDS makes CMake re-scan when files change.
	file(GLOB_RECURSE SWIFT_SOURCES CONFIGURE_DEPENDS
		${CMAKE_SOURCE_DIR}/Swift/*.swift
	)
	armsx2_require_files(SWIFT_SOURCES)

	configure_file("${CMAKE_SOURCE_DIR}/Info.plist.in" "${CMAKE_BINARY_DIR}/Info.plist" @ONLY)

	if(ARMSX2_REAL_DEVICE OR ARMSX2_MAC_CATALYST)
		set(ARMSX2_CODE_SIGNING_ALLOWED "YES")
		set(ARMSX2_CODE_SIGNING_REQUIRED "YES")
		set(ARMSX2_CODE_SIGN_STYLE "Automatic")
		set(ARMSX2_CODE_SIGN_IDENTITY "Apple Development")
	else()
		set(ARMSX2_CODE_SIGNING_ALLOWED "NO")
		set(ARMSX2_CODE_SIGNING_REQUIRED "NO")
		set(ARMSX2_CODE_SIGN_STYLE "Manual")
		set(ARMSX2_CODE_SIGN_IDENTITY "")
	endif()

	if(ARMSX2_ENABLE_JIT_ENTITLEMENTS)
		set(ARMSX2_CODE_SIGN_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/Entitlements.plist")
	else()
		set(ARMSX2_CODE_SIGN_ENTITLEMENTS "")
	endif()

	if(ARMSX2_MAC_CATALYST)
		set(ARMSX2_SUPPORTS_MACCATALYST "YES")
		set(ARMSX2_DERIVE_MACCATALYST_BUNDLE_IDENTIFIER "NO")
		set(ARMSX2_SUPPORTED_PLATFORMS "iphoneos iphonesimulator macosx")
		set(ARMSX2_TARGETED_DEVICE_FAMILY "1,2,6")
		set(ARMSX2_ENABLE_HARDENED_RUNTIME "YES")
		# Catalyst takes its macOS floor from this setting; iOS 18.0 is macOS 15.0.
		set(ARMSX2_APP_IPHONEOS_DEPLOYMENT_TARGET "18.0")
	else()
		set(ARMSX2_SUPPORTS_MACCATALYST "NO")
		set(ARMSX2_DERIVE_MACCATALYST_BUNDLE_IDENTIFIER "YES")
		set(ARMSX2_SUPPORTED_PLATFORMS "iphoneos iphonesimulator")
		set(ARMSX2_TARGETED_DEVICE_FAMILY "1,2")
		set(ARMSX2_ENABLE_HARDENED_RUNTIME "NO")
		set(ARMSX2_APP_IPHONEOS_DEPLOYMENT_TARGET "${ARMSX2_IOS_DEPLOYMENT_TARGET}")
	endif()

	set(IOS_RUNTIME_SOURCES
		Runtime/ios_main.mm
		Runtime/AppDelegate.mm
		Runtime/GamepadHaptics.mm
		Runtime/HostImpls.mm
		Runtime/PlaySoundAsync.mm
		Runtime/SceneDelegate.mm
	)
	file(GLOB IOS_BRIDGE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Bridge/*.mm)
	armsx2_require_files(IOS_BRIDGE_SOURCES)
	# Only the bridge. The files in IOS_RUNTIME_SOURCES still release by hand, and
	# test_ios_airplay.py pins one of those lines out of SceneDelegate.mm.
	set_source_files_properties(${IOS_BRIDGE_SOURCES} PROPERTIES COMPILE_FLAGS "-fobjc-arc")
	add_executable(ARMSX2iOS ${IOS_RUNTIME_SOURCES} ${IOS_BRIDGE_SOURCES} ${SWIFT_SOURCES})
	set_target_properties(ARMSX2iOS PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_GUI_IDENTIFIER "${ARMSX2_BUNDLE_IDENTIFIER}"
		MACOSX_BUNDLE_BUNDLE_NAME "ARMSX2 iOS"
		MACOSX_BUNDLE_BUNDLE_VERSION "${ARMSX2_BUILD_NUMBER}"
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${ARMSX2_VERSION}"
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/Info.plist"
		MACOSX_BUNDLE_EXECUTABLE_NAME "ARMSX2iOS"
		OUTPUT_NAME "ARMSX2iOS"
		XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${ARMSX2_BUNDLE_IDENTIFIER}"
		XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${ARMSX2_TARGETED_DEVICE_FAMILY}"
		XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${ARMSX2_APP_IPHONEOS_DEPLOYMENT_TARGET}"
		XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "${ARMSX2_MACOS_DEPLOYMENT_TARGET}"
		XCODE_ATTRIBUTE_SDKROOT "${CMAKE_OSX_SYSROOT}"
		XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "${ARMSX2_SUPPORTED_PLATFORMS}"
		XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST "${ARMSX2_SUPPORTS_MACCATALYST}"
		XCODE_ATTRIBUTE_DERIVE_MACCATALYST_PRODUCT_BUNDLE_IDENTIFIER "${ARMSX2_DERIVE_MACCATALYST_BUNDLE_IDENTIFIER}"
		XCODE_ATTRIBUTE_SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD "NO"
		XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME "${ARMSX2_ENABLE_HARDENED_RUNTIME}"
		# Xcode strips dead code by default for iOS but not for Mac Catalyst.
		XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING "YES"
		XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "${ARMSX2_CODE_SIGNING_ALLOWED}"
		XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "${ARMSX2_CODE_SIGNING_REQUIRED}"
		XCODE_ATTRIBUTE_CODE_SIGN_STYLE "${ARMSX2_CODE_SIGN_STYLE}"
		XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${ARMSX2_CODE_SIGN_IDENTITY}"
		XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${ARMSX2_DEVELOPMENT_TEAM}"
		XCODE_ATTRIBUTE_PROVISIONING_PROFILE_SPECIFIER ""
		XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${ARMSX2_CODE_SIGN_ENTITLEMENTS}"
			XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_SOURCE_DIR}/Bridge/ARMSX2-Bridging-Header.h"
			XCODE_ATTRIBUTE_SWIFT_VERSION "6.0"
			# Debug compiles Swift per file; Release is whole-module, as on base. If a
			# Release build runs out of frontend memory, per-file goes back for Release.
			XCODE_ATTRIBUTE_SWIFT_COMPILATION_MODE "$<IF:$<CONFIG:Debug>,singlefile,wholemodule>"
			XCODE_ATTRIBUTE_SWIFT_ENABLE_BATCH_MODE "$<IF:$<CONFIG:Debug>,NO,YES>"
			XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL "$<IF:$<CONFIG:Debug>,-Onone,-O>"
			XCODE_ATTRIBUTE_COMPILER_INDEX_STORE_ENABLE "NO"
			XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES "YES"
			XCODE_ATTRIBUTE_ENABLE_ON_DEMAND_RESOURCES "NO"
			XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks /usr/lib/swift"
			XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS "$(inherited) $(TOOLCHAIN_DIR)/usr/lib/swift/$(PLATFORM_NAME) /usr/lib/swift"
			XCODE_ATTRIBUTE_OTHER_SWIFT_FLAGS ""
			XCODE_ATTRIBUTE_MTL_HEADER_SEARCH_PATHS "${ARMSX2_ROOT}/pcsx2/GS/Renderers/Metal ${ARMSX2_ROOT}/3rdparty/include"
			XCODE_ATTRIBUTE_MTL_ENABLE_DEBUG_INFO "$<IF:$<CONFIG:Debug>,INCLUDE_SOURCE,>"
		)

		set(BOOT_SPLASH_VIDEO "${CMAKE_SOURCE_DIR}/assets/video/boot_intro.mp4")
		set(IOS_APP_ICON_FILES
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-20.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-29.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-40.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-58.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-60.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-76.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-80.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-87.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-120.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-152.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-167.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-180.png"
			"${CMAKE_SOURCE_DIR}/assets/Assets.xcassets/AppIcon.appiconset/icon-1024.png"
		)
		foreach(IOS_APP_ICON_FILE IN LISTS IOS_APP_ICON_FILES)
			target_sources(ARMSX2iOS PRIVATE "${IOS_APP_ICON_FILE}")
			set_source_files_properties("${IOS_APP_ICON_FILE}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
		endforeach()
		# Alternate app icons (user-selectable from Settings). Bundled as loose
		# Resources and referenced by name in Info.plist CFBundleAlternateIcons.
		file(GLOB IOS_ALTERNATE_APP_ICON_FILES "${CMAKE_SOURCE_DIR}/assets/app_icons/alternate_app_icons/*.png")
		armsx2_require_files(IOS_ALTERNATE_APP_ICON_FILES)
		foreach(IOS_ALTERNATE_APP_ICON_FILE IN LISTS IOS_ALTERNATE_APP_ICON_FILES)
			if(EXISTS "${IOS_ALTERNATE_APP_ICON_FILE}")
				target_sources(ARMSX2iOS PRIVATE "${IOS_ALTERNATE_APP_ICON_FILE}")
				set_source_files_properties("${IOS_ALTERNATE_APP_ICON_FILE}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
			endif()
		endforeach()
		# Preview thumbnails for the SwiftUI icon picker.
		file(GLOB IOS_ALTERNATE_APP_ICON_PREVIEWS "${CMAKE_SOURCE_DIR}/assets/app_icons/alternate_app_icons/previews/*.png")
		armsx2_require_files(IOS_ALTERNATE_APP_ICON_PREVIEWS)
		foreach(IOS_ALTERNATE_APP_ICON_PREVIEW IN LISTS IOS_ALTERNATE_APP_ICON_PREVIEWS)
			if(EXISTS "${IOS_ALTERNATE_APP_ICON_PREVIEW}")
				target_sources(ARMSX2iOS PRIVATE "${IOS_ALTERNATE_APP_ICON_PREVIEW}")
				set_source_files_properties("${IOS_ALTERNATE_APP_ICON_PREVIEW}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
			endif()
		endforeach()
		# Full-resolution icon images exported for Home Screen shortcuts.
		file(GLOB IOS_ALTERNATE_APP_ICON_EXPORTS "${CMAKE_SOURCE_DIR}/assets/app_icons/alternate_app_icons/exports/*.png")
		armsx2_require_files(IOS_ALTERNATE_APP_ICON_EXPORTS)
		foreach(IOS_ALTERNATE_APP_ICON_EXPORT IN LISTS IOS_ALTERNATE_APP_ICON_EXPORTS)
			if(EXISTS "${IOS_ALTERNATE_APP_ICON_EXPORT}")
				target_sources(ARMSX2iOS PRIVATE "${IOS_ALTERNATE_APP_ICON_EXPORT}")
				set_source_files_properties("${IOS_ALTERNATE_APP_ICON_EXPORT}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
			endif()
		endforeach()
		target_sources(ARMSX2iOS PRIVATE "${BOOT_SPLASH_VIDEO}")
		set_source_files_properties("${BOOT_SPLASH_VIDEO}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

		# UI defaults come directly from the Android resource tree. There is no
		# duplicate iOS audio pack to become stale; startup remains custom-pack-only.
		set(IOS_DEFAULT_UI_AUDIO_FILES
			"${ARMSX2_ROOT}/platforms/android/app/src/main/res/raw/sfx_nav_a.wav"
			"${ARMSX2_ROOT}/platforms/android/app/src/main/res/raw/library_music.m4a"
			"${ARMSX2_ROOT}/platforms/android/app/src/main/res/raw/pause_music.mp3"
			"${ARMSX2_ROOT}/platforms/android/app/src/main/res/raw/sfx_reset.wav"
			"${ARMSX2_ROOT}/platforms/android/app/src/main/res/raw/sfx_popup_close.wav"
			"${ARMSX2_ROOT}/platforms/android/app/src/main/res/raw/sfx_popup_open.wav"
			"${ARMSX2_ROOT}/platforms/android/app/src/main/res/raw/sfx_nav_b.wav"
			"${ARMSX2_ROOT}/platforms/android/app/src/main/res/raw/sfx_toggle_off.wav"
			"${ARMSX2_ROOT}/platforms/android/app/src/main/res/raw/sfx_toggle_on.wav"
		)
		target_sources(ARMSX2iOS PRIVATE ${IOS_DEFAULT_UI_AUDIO_FILES})
		set_source_files_properties(${IOS_DEFAULT_UI_AUDIO_FILES} PROPERTIES
			MACOSX_PACKAGE_LOCATION "Resources/ui_audio/default")

		file(GLOB IOS_CONTROLLER_ICON_FILES "${CMAKE_SOURCE_DIR}/assets/controller_icons/*.png")
		armsx2_require_files(IOS_CONTROLLER_ICON_FILES)
		target_sources(ARMSX2iOS PRIVATE ${IOS_CONTROLLER_ICON_FILES})
		set_source_files_properties(${IOS_CONTROLLER_ICON_FILES} PROPERTIES
			MACOSX_PACKAGE_LOCATION Resources)

		file(GLOB_RECURSE IOS_CONTROLLER_SKIN_FILES "${CMAKE_SOURCE_DIR}/assets/controller_skins/*.png")
		armsx2_require_files(IOS_CONTROLLER_SKIN_FILES)
		foreach(IOS_CONTROLLER_SKIN_FILE IN LISTS IOS_CONTROLLER_SKIN_FILES)
			file(RELATIVE_PATH IOS_CONTROLLER_SKIN_RELATIVE_FILE
				"${CMAKE_SOURCE_DIR}/assets"
				"${IOS_CONTROLLER_SKIN_FILE}")
			get_filename_component(IOS_CONTROLLER_SKIN_RELATIVE_DIR
				"${IOS_CONTROLLER_SKIN_RELATIVE_FILE}" DIRECTORY)
			target_sources(ARMSX2iOS PRIVATE "${IOS_CONTROLLER_SKIN_FILE}")
			set_source_files_properties("${IOS_CONTROLLER_SKIN_FILE}" PROPERTIES
				MACOSX_PACKAGE_LOCATION "Resources/${IOS_CONTROLLER_SKIN_RELATIVE_DIR}")
		endforeach()

		# A .slangp names its stages by relative path, so the tree has to survive the copy;
		# a flattened bundle breaks every preset. Staged under "Resources/..." like the
		# controller skins above, which iOS then flattens to the .app root — the shipped
		# path is <app>/shaders/..., with no Resources/ component.
		file(GLOB_RECURSE IOS_SHADER_PRESET_FILES CONFIGURE_DEPENDS
			"${CMAKE_SOURCE_DIR}/assets/shaders/*")
		armsx2_require_files(IOS_SHADER_PRESET_FILES)
		foreach(IOS_SHADER_PRESET_FILE IN LISTS IOS_SHADER_PRESET_FILES)
			file(RELATIVE_PATH IOS_SHADER_PRESET_RELATIVE_FILE
				"${CMAKE_SOURCE_DIR}/assets"
				"${IOS_SHADER_PRESET_FILE}")
			get_filename_component(IOS_SHADER_PRESET_RELATIVE_DIR
				"${IOS_SHADER_PRESET_RELATIVE_FILE}" DIRECTORY)
			target_sources(ARMSX2iOS PRIVATE "${IOS_SHADER_PRESET_FILE}")
			set_source_files_properties("${IOS_SHADER_PRESET_FILE}" PROPERTIES
				MACOSX_PACKAGE_LOCATION "Resources/${IOS_SHADER_PRESET_RELATIVE_DIR}")
		endforeach()

	set(ARMSX2_APPLE_FRAMEWORKS
		"-framework UIKit"
		"-framework AVFoundation"
		"-framework Metal"
		"-framework MetalKit"
		"-framework QuartzCore"
		"-framework CoreText"
		"-framework CoreGraphics"
		"-framework ImageIO"
		"-framework GameController"
		"-framework CoreHaptics"
	)
	if(NOT ARMSX2_MAC_CATALYST)
		list(APPEND ARMSX2_APPLE_FRAMEWORKS "-framework OpenGLES")
	endif()

	target_link_libraries(ARMSX2iOS PRIVATE
		armsx2core::PCSX2
		SDL3::SDL3-static
		${ARMSX2_APPLE_FRAMEWORKS}
	)

	if(ARMSX2_HAVE_LIBRASHADER)
		target_link_libraries(ARMSX2iOS PRIVATE librashader)
		# The core's define stops at its archives; the bridge's capability query needs its own.
		target_compile_definitions(ARMSX2iOS PRIVATE ARMSX2_HAS_LIBRASHADER=1)
	endif()

	add_custom_target(armsx2_core
		COMMAND ${CMAKE_COMMAND} -E env ARMSX2_CMAKE=${CMAKE_COMMAND} ARMSX2_NINJA=${ARMSX2_NINJA}
			ARMSX2_APP_LIBRASHADER=${ARMSX2_HAVE_LIBRASHADER}
			${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ios-core.sh ${ARMSX2_CORE_PLATFORM}
		COMMENT "Building the core in build/core-${ARMSX2_CORE_PLATFORM}"
	)
	add_dependencies(ARMSX2iOS armsx2_core)

	find_program(ARMSX2_PYTHON3 python3)
	if(NOT ARMSX2_PYTHON3)
		message(FATAL_ERROR "python3 not found; the bridge method check needs it")
	endif()
	# A method the header declares but no bridge file implements only fails at run time.
	add_custom_command(TARGET ARMSX2iOS POST_BUILD
		COMMAND ${ARMSX2_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_bridge_methods.py
			${CMAKE_CURRENT_SOURCE_DIR}/Bridge/ARMSX2Bridge.h $<TARGET_FILE:ARMSX2iOS>
		VERBATIM
	)

	target_compile_options(ARMSX2iOS PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-pipe>)
	# With OBJCXX not enabled, CMake builds this target's .mm files as CXX. The core keeps its own warnings.
	target_compile_options(ARMSX2iOS PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wunused-function;-Wunused-variable;-Wunused-private-field;-Wunused-but-set-variable;-Wunused-lambda-capture;-Wunused-local-typedef;-Wunused-label>")

	# The hash used to be resolved here, which meant it stuck at whatever cmake
	# last saw and a build after a commit reported the wrong revision. Resolve it
	# into a header on every build instead. The version stays a define because it
	# lives in this file, so touching it always reconfigures anyway.
	set(ARMSX2_GIT_HASH_HEADER "${CMAKE_CURRENT_BINARY_DIR}/armsx2_git_hash.h")
	# Seed it now so the header exists even before the target below has run once.
	execute_process(COMMAND ${CMAKE_COMMAND}
		-DGIT_WORKING_DIR=${CMAKE_SOURCE_DIR}
		-DOUTPUT_FILE=${ARMSX2_GIT_HASH_HEADER}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteGitHash.cmake)
	add_custom_target(armsx2_git_hash ALL
		BYPRODUCTS "${ARMSX2_GIT_HASH_HEADER}"
		COMMAND ${CMAKE_COMMAND}
			-DGIT_WORKING_DIR=${CMAKE_SOURCE_DIR}
			-DOUTPUT_FILE=${ARMSX2_GIT_HASH_HEADER}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteGitHash.cmake
		COMMENT "Resolving git hash"
	)
	add_dependencies(ARMSX2iOS armsx2_git_hash)

	target_compile_definitions(ARMSX2iOS PRIVATE
		PCSX2_NO_PCAP=1
		ARMSX2_VERSION_STR="${ARMSX2_VERSION}"
	)
	if(ARMSX2_MAC_CATALYST)
		target_compile_definitions(ARMSX2iOS PRIVATE ARMSX2_MAC_CATALYST_BUILD=1)
	endif()

	target_include_directories(ARMSX2iOS PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}
		${CMAKE_SOURCE_DIR}/Bridge
		${CMAKE_SOURCE_DIR}/Runtime
		${ARMSX2_ROOT}
		${ARMSX2_ROOT}/common
		${ARMSX2_ROOT}/pcsx2
		${ARMSX2_ROOT}/pcsx2/GS/Renderers/Metal
		${ARMSX2_ROOT}/3rdparty/SDL3/include
		${ARMSX2_ROOT}/3rdparty/fmt/include
		${ARMSX2_ROOT}/3rdparty/fast_float/include
		${ARMSX2_ROOT}/3rdparty/vixl/include
		${ARMSX2_ROOT}/3rdparty/include
		${ARMSX2_ROOT}/3rdparty/vulkan/include
		${ARMSX2_ROOT}/3rdparty/glslang/glslang
		${ARMSX2_ROOT}/3rdparty/glslang/include
		${ARMSX2_CORE_DIR}/common/include
		${ARMSX2_CORE_DIR}/pcsx2
		${ARMSX2_ROOT}/3rdparty/simpleini/include
		${ARMSX2_ROOT}/3rdparty/imgui/include
		${ARMSX2_ROOT}/3rdparty/rapidyaml/include
		${ARMSX2_ROOT}/3rdparty/cpuinfo/include
		${ARMSX2_ROOT}/3rdparty/libchdr/include
		${ARMSX2_ROOT}/3rdparty/libzip/lib
		${ARMSX2_CORE_DIR}/3rdparty/libzip
		${ARMSX2_ROOT}/3rdparty/cubeb/include
		${ARMSX2_ROOT}/3rdparty/rcheevos/include
		${ARMSX2_ROOT}/3rdparty/freesurround/include
		${ARMSX2_ROOT}/3rdparty/freetype/include
		${ARMSX2_ROOT}/3rdparty/plutosvg1/source
		${ARMSX2_ROOT}/3rdparty/plutosvg1/plutovg/include
		${ARMSX2_CORE_DIR}/3rdparty/freetype/include
		${ARMSX2_ROOT}/3rdparty/soundtouch/soundtouch
		${ARMSX2_ROOT}/3rdparty/lz4/lz4/lib
		${ARMSX2_ROOT}/3rdparty/lzma/include
		${ARMSX2_ANDROID_VENDORED}/zstd/lib
		${ARMSX2_ROOT}/3rdparty/demangler/include
		${ARMSX2_ROOT}/3rdparty/ccc/src
		${ARMSX2_CORE_DIR}/3rdparty/SDL3/include-revision
	)

	# Runtime resources come from the canonical bin/resources tree (shared with the
	# desktop and Android builds), not a committed iOS copy. dx11 shaders are
	# excluded — iOS renders with Metal and never compiles them. The mobile GameDB
	# overlay is bundled alongside so the shared GameDatabase override loader applies
	# the same tiler-GPU tuning at runtime (converging iOS onto the runtime-overlay
	# model, retiring the old build-time gamedb-convert.py merge). patches.zip is the
	# only genuine iOS-only extra still committed under assets/resources.
	set(ARMSX2_SHARED_RESOURCES "${ARMSX2_ROOT}/bin/resources")
	file(GLOB METAL_SHADERS "${ARMSX2_ROOT}/pcsx2/GS/Renderers/Metal/*.metal")
	armsx2_require_files(METAL_SHADERS)
	file(GLOB FONTS "${ARMSX2_SHARED_RESOURCES}/fonts/*.ttf" "${ARMSX2_SHARED_RESOURCES}/fonts/*.otf")
	armsx2_require_files(FONTS)
	file(GLOB_RECURSE IOS_SHADER_RESOURCES "${ARMSX2_SHARED_RESOURCES}/shaders/*")
	list(FILTER IOS_SHADER_RESOURCES EXCLUDE REGEX "/dx11/")
	armsx2_require_files(IOS_SHADER_RESOURCES)
	file(GLOB_RECURSE IOS_SOUND_RESOURCES "${ARMSX2_SHARED_RESOURCES}/sounds/*")
	armsx2_require_files(IOS_SOUND_RESOURCES)
	# The texture upscaler's filter set, read from <Resources>/upscale/raisr/ps2 at run time.
	file(GLOB_RECURSE IOS_UPSCALE_RESOURCES "${ARMSX2_SHARED_RESOURCES}/upscale/*")
	armsx2_require_files(IOS_UPSCALE_RESOURCES)
	set(IOS_TOP_LEVEL_RESOURCES
		"${ARMSX2_SHARED_RESOURCES}/GameIndex.yaml"
		"${ARMSX2_SHARED_RESOURCES}/RedumpDatabase.yaml"
		"${ARMSX2_SHARED_RESOURCES}/game_controller_db.txt"
		"${ARMSX2_ROOT}/bin/resources-overlay/armsx2_overrides.yaml"
		"${CMAKE_SOURCE_DIR}/assets/resources/patches.zip"
		"${CMAKE_SOURCE_DIR}/assets/resources/Localizable.xcstrings"
		"${CMAKE_SOURCE_DIR}/assets/resources/Purple-Milk.armsx2-theme.json"
		"${CMAKE_SOURCE_DIR}/assets/resources/Ender-Pearl-Pul.armsx2-theme.json"
	)

	target_sources(ARMSX2iOS PRIVATE ${METAL_SHADERS} ${FONTS} ${IOS_SHADER_RESOURCES})
	set_source_files_properties(${METAL_SHADERS} PROPERTIES
		XCODE_EXPLICIT_FILE_TYPE "sourcecode.metal"
		MACOSX_PACKAGE_LOCATION Resources
	)
	set_source_files_properties(${FONTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/fonts)
	foreach(IOS_TOP_LEVEL_RESOURCE IN LISTS IOS_TOP_LEVEL_RESOURCES)
		target_sources(ARMSX2iOS PRIVATE "${IOS_TOP_LEVEL_RESOURCE}")
		set_source_files_properties("${IOS_TOP_LEVEL_RESOURCE}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
	endforeach()
	foreach(IOS_SHADER_RESOURCE IN LISTS IOS_SHADER_RESOURCES)
		get_filename_component(IOS_SHADER_DIR "${IOS_SHADER_RESOURCE}" DIRECTORY)
		file(RELATIVE_PATH IOS_SHADER_SUBDIR "${ARMSX2_SHARED_RESOURCES}" "${IOS_SHADER_DIR}")
		set_source_files_properties("${IOS_SHADER_RESOURCE}" PROPERTIES
			MACOSX_PACKAGE_LOCATION "Resources/${IOS_SHADER_SUBDIR}")
	endforeach()
	target_sources(ARMSX2iOS PRIVATE ${IOS_SOUND_RESOURCES})
	foreach(IOS_SOUND_RESOURCE IN LISTS IOS_SOUND_RESOURCES)
		get_filename_component(IOS_SOUND_DIR "${IOS_SOUND_RESOURCE}" DIRECTORY)
		file(RELATIVE_PATH IOS_SOUND_SUBDIR "${ARMSX2_SHARED_RESOURCES}" "${IOS_SOUND_DIR}")
		set_source_files_properties("${IOS_SOUND_RESOURCE}" PROPERTIES
			MACOSX_PACKAGE_LOCATION "Resources/${IOS_SOUND_SUBDIR}")
	endforeach()
	target_sources(ARMSX2iOS PRIVATE ${IOS_UPSCALE_RESOURCES})
	foreach(IOS_UPSCALE_RESOURCE IN LISTS IOS_UPSCALE_RESOURCES)
		get_filename_component(IOS_UPSCALE_DIR "${IOS_UPSCALE_RESOURCE}" DIRECTORY)
		file(RELATIVE_PATH IOS_UPSCALE_SUBDIR "${ARMSX2_SHARED_RESOURCES}" "${IOS_UPSCALE_DIR}")
		set_source_files_properties("${IOS_UPSCALE_RESOURCE}" PROPERTIES
			MACOSX_PACKAGE_LOCATION "Resources/${IOS_UPSCALE_SUBDIR}")
	endforeach()

	if(CMAKE_GENERATOR STREQUAL "Xcode" AND ARMSX2_MAC_CATALYST)
		function(armsx2_disable_signing_for_non_app_targets directory)
			get_property(armsx2_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
			foreach(armsx2_target IN LISTS armsx2_targets)
				if(NOT armsx2_target STREQUAL "ARMSX2iOS")
					set_target_properties(${armsx2_target} PROPERTIES
						XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
						XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
						XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
						XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
						XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
						XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ""
					)
				endif()
			endforeach()

			get_property(armsx2_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
			foreach(armsx2_subdirectory IN LISTS armsx2_subdirectories)
				armsx2_disable_signing_for_non_app_targets("${armsx2_subdirectory}")
			endforeach()
		endfunction()

		armsx2_disable_signing_for_non_app_targets("${CMAKE_SOURCE_DIR}")
	endif()
endif()

#-------------------------------------------------------------------------------
if(NOT IS_SUPPORTED_COMPILER)
	message(WARNING "
*************** UNSUPPORTED CONFIGURATION ***************
You are not compiling PCSX2 with a supported compiler.
It may not even build successfully.
PCSX2 only supports the Clang and MSVC compilers.
No support will be provided, continue at your own risk.
*********************************************************")
endif()
