#-------------------------------------------------------------------------------
#                       Search all libraries on the system
#-------------------------------------------------------------------------------
find_package(Git)

# Require threads on all OSes.
find_package(Threads REQUIRED)

# Dependency libraries.
# On macOS, Mono.framework contains an ancient version of libpng.  We don't want that.
# Avoid it by telling cmake to avoid finding frameworks while we search for libpng.
set(FIND_FRAMEWORK_BACKUP ${CMAKE_FIND_FRAMEWORK})
set(CMAKE_FIND_FRAMEWORK NEVER)
find_package(PNG 1.6.40 REQUIRED)
find_package(JPEG REQUIRED) # No version because flatpak uses libjpeg-turbo.
find_package(ZLIB REQUIRED) # v1.3, but Mac uses the SDK version.
find_package(Zstd 1.5.5 REQUIRED)
find_package(LZ4 REQUIRED)
find_package(WebP REQUIRED) # v1.3.2, spews an error on Linux because no pkg-config.
find_package(SDL3 3.2.6 REQUIRED)
find_package(Freetype 2.10 REQUIRED) # 2.10 is the first with COLRv0 support, which we need for rendering emoji

# No CONFIG keyword above, so that's Module-mode find_package(Freetype) resolving
# through CMake's own FindFreetype.cmake, which knows nothing of HarfBuzz. A shared
# libfreetype.so records its own HarfBuzz dependency and needs nothing from us; a
# static libfreetype.a (build-dependencies-runner.sh's ARMSX2_DEPS_STATIC=1 path,
# built with FT_REQUIRE_HARFBUZZ=TRUE) genuinely calls hb_* symbols and does not,
# same as libwebp/sharpyuv above (cmake/FindWebP.cmake). Gate on the archive we
# actually found being static, so every shared resolution (Qt desktop, macOS,
# Windows) stays a no-op.
#
# Static does not always mean it calls HarfBuzz, and the two prefixes we build
# differ: the Android one
# (.github/workflows/scripts/android/build-dependencies.sh) and the iOS one
# (platforms/ios/.../SearchForStuff.cmake) build FreeType with
# FT_DISABLE_HARFBUZZ=ON and ship no HarfBuzz at all, so requiring one there
# fails a build whose libfreetype.a wants nothing - which is exactly how the
# android-arm64-v8a job died. So ask the archive instead of assuming: nm lists
# what it leaves undefined, and an FT_REQUIRE_HARFBUZZ build leaves hb_* there.
get_filename_component(FREETYPE_LIBRARY_EXT "${FREETYPE_LIBRARY}" EXT)
if (NOT WIN32 AND FREETYPE_LIBRARY_EXT STREQUAL "${CMAKE_STATIC_LIBRARY_SUFFIX}")
	# CMAKE_NM is what the toolchain (including the NDK's) points at; nm and
	# llvm-nm are the fallbacks for a plain host build. Mach-O prefixes the
	# underscore, hence the optional one in the pattern.
	set(FREETYPE_NEEDS_HARFBUZZ FALSE)
	find_program(ARMSX2_NM NAMES "${CMAKE_NM}" nm llvm-nm)
	if (ARMSX2_NM)
		execute_process(COMMAND "${ARMSX2_NM}" -u "${FREETYPE_LIBRARY}"
			OUTPUT_VARIABLE FREETYPE_UNDEFINED
			ERROR_VARIABLE FREETYPE_NM_ERROR
			RESULT_VARIABLE FREETYPE_NM_RESULT
			OUTPUT_STRIP_TRAILING_WHITESPACE)
		if (NOT FREETYPE_NM_RESULT EQUAL 0)
			# Cannot tell: keep the old assumption rather than hand the linker an
			# archive with unresolved hb_* in it.
			message(STATUS "Could not run ${ARMSX2_NM} on ${FREETYPE_LIBRARY}, assuming it needs HarfBuzz")
			set(FREETYPE_NEEDS_HARFBUZZ TRUE)
		elseif (FREETYPE_UNDEFINED MATCHES "[ \t]_?hb_")
			set(FREETYPE_NEEDS_HARFBUZZ TRUE)
		endif()
		unset(FREETYPE_UNDEFINED)
		unset(FREETYPE_NM_ERROR)
		unset(FREETYPE_NM_RESULT)
	else()
		message(STATUS "No nm to inspect ${FREETYPE_LIBRARY} with, assuming it needs HarfBuzz")
		set(FREETYPE_NEEDS_HARFBUZZ TRUE)
	endif()
endif()

if (FREETYPE_NEEDS_HARFBUZZ)
	# Archive first. The static prefix has libharfbuzz.a, and resolving a system
	# libharfbuzz.so here would put a NEEDED entry back onto a binary whose whole
	# purpose is carrying everything it needs.
	find_library(HARFBUZZ_LIBRARY
		NAMES "${CMAKE_STATIC_LIBRARY_PREFIX}harfbuzz${CMAKE_STATIC_LIBRARY_SUFFIX}"
		      harfbuzz libharfbuzz)
	if (NOT HARFBUZZ_LIBRARY)
		message(FATAL_ERROR "${FREETYPE_LIBRARY} leaves hb_* undefined but no HarfBuzz was found: "
			"a static libfreetype.a built with FT_REQUIRE_HARFBUZZ calls into HarfBuzz "
			"directly and won't resolve without it.")
	endif()
	# HarfBuzz, then FreeType again. The two archives call into each other, and a
	# single-pass linker only resolves that if whichever still owes symbols comes
	# last. Both orders worked on binutils 2.44; CI runs an older one, and being
	# wrong here leaves undefined symbols in a .so that links clean and then
	# fails at dlopen, which is the failure this whole change exists to remove.
	set_property(TARGET Freetype::Freetype APPEND PROPERTY
		INTERFACE_LINK_LIBRARIES "${HARFBUZZ_LIBRARY}" "${FREETYPE_LIBRARY}")
endif()
unset(FREETYPE_NEEDS_HARFBUZZ)
unset(FREETYPE_LIBRARY_EXT)
find_package(plutovg 1.1.0 REQUIRED)
find_package(plutosvg 0.0.7 REQUIRED)
# NOT taking upstream's find_package(ryml): we re-vendor rapidyaml in-tree (see the
# add_subdirectory note further down) precisely so handheld and cross builds stay
# self-contained. Upstream un-bundled it; we deliberately did not follow.
if (WIN32)
	find_package(DirectX-Headers 1.618.1 REQUIRED)
endif()

if(USE_VULKAN)
	find_package(Shaderc REQUIRED)
endif()

# Platform-specific dependencies.
if (WIN32)
	add_subdirectory(3rdparty/D3D12MemAlloc EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/winpixeventruntime EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/winwil EXCLUDE_FROM_ALL)
	find_package(Vtune)
else()
	# Neither iOS nor tvOS has a libcurl to link against, and common/CMakeLists
	# already leaves HTTPDownloaderCurl.cpp out there for that reason -
	# HTTPDownloader::Create() returns nothing and its callers (achievements,
	# cover downloads) treat the downloader as unavailable. So this must not be
	# REQUIRED there either; it was the first thing an iOS configure stopped on.
	if(NOT IOS)
		find_package(CURL REQUIRED)
		find_package(PCAP REQUIRED)
	endif()
	find_package(Vtune)

	## Use CheckLib package to find module
	include(CheckLib)

	if(UNIX AND NOT APPLE)
		# Android is UNIX AND NOT APPLE, but it is not a desktop: it has no
		# fontconfig and no D-Bus session to inhibit a screensaver on, and the
		# only thing built there is the libretro core, which asks for neither.
		if(NOT ANDROID)
			find_package(Fontconfig REQUIRED)
		endif()
		if(LINUX)
			check_lib(LIBUDEV libudev libudev.h)
		endif()

		if(X11_API)
			find_package(X11 REQUIRED)
			if (NOT X11_Xrandr_FOUND)
				message(FATAL_ERROR "XRandR extension is required")
			endif()
		endif()

		if(WAYLAND_API)
			find_package(ECM REQUIRED NO_MODULE)
			list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")
			find_package(Wayland REQUIRED Egl)
		endif()

		if(USE_BACKTRACE)
			find_package(Libbacktrace REQUIRED)
		endif()

		if(NOT ANDROID)
			find_package(PkgConfig REQUIRED)
			pkg_check_modules(DBUS REQUIRED dbus-1)
		endif()
	endif()
endif()

set(CMAKE_FIND_FRAMEWORK ${FIND_FRAMEWORK_BACKUP})

add_subdirectory(3rdparty/fast_float EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/libretro EXCLUDE_FROM_ALL)
# rapidyaml re-vendored in-tree (fork-local; upstream 0beb18c9e un-bundled it in
# favour of a system ryml). Keeps the build self-contained for handheld/cross builds.
add_subdirectory(3rdparty/rapidyaml EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/lzma EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/libchdr EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(libchdr)
add_subdirectory(3rdparty/soundtouch EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/simpleini EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/imgui EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/cpuinfo EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(cpuinfo)
add_subdirectory(3rdparty/libzip EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/rcheevos EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/rapidjson EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/discord-rpc EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/freesurround EXCLUDE_FROM_ALL)

if(USE_OPENGL)
	add_subdirectory(3rdparty/glad EXCLUDE_FROM_ALL)
endif()

if(USE_VULKAN)
	add_subdirectory(3rdparty/vulkan EXCLUDE_FROM_ALL)
endif()

add_subdirectory(3rdparty/cubeb EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(cubeb)
disable_compiler_warnings_for_target(speex)

# Find the Qt components that we need.
if(ENABLE_QT_UI)
	find_package(Qt6 6.10.1 COMPONENTS CoreTools Core GuiTools Gui WidgetsTools Widgets LinguistTools REQUIRED)

	if(NOT WIN32 AND NOT APPLE)
		if (Qt6_VERSION VERSION_GREATER_EQUAL 6.10.0)
			find_package(Qt6 COMPONENTS CorePrivate GuiPrivate WidgetsPrivate REQUIRED)
		endif()
	endif()

	# The docking system for the debugger.
	if(ENABLE_QT_DEBUGGER)
		find_package(KDDockWidgets-qt6 2.3.0 REQUIRED)
	endif()
endif()

if(WIN32)
	add_subdirectory(3rdparty/rainterface EXCLUDE_FROM_ALL)
endif()

# Demangler for the debugger.
add_subdirectory(3rdparty/demangler EXCLUDE_FROM_ALL)

# Symbol table parser.
add_subdirectory(3rdparty/ccc EXCLUDE_FROM_ALL)

# Architecture-specific.
if(ARCH_X86)
	add_subdirectory(3rdparty/zydis EXCLUDE_FROM_ALL)
elseif(ARCH_ARM64)
	add_subdirectory(3rdparty/vixl EXCLUDE_FROM_ALL)
endif()

# Prevent fmt from being built with exceptions, or being thrown at call sites.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DFMT_USE_EXCEPTIONS=0 -DFMT_USE_RTTI=0")
add_subdirectory(3rdparty/fmt EXCLUDE_FROM_ALL)

# Deliberately at the end. We don't want to set the flag on third-party projects.
if(MSVC)
	# Don't warn about "deprecated" POSIX functions.
	add_definitions("-D_CRT_NONSTDC_NO_WARNINGS" "-D_CRT_SECURE_NO_WARNINGS" "-DCRT_SECURE_NO_DEPRECATE")
endif()
