#!/bin/bash
#
# Builds the dependency set for the Apple *embedded* platforms - iOS and tvOS -
# for the libretro buildbot. build-dependencies-slice.sh next to this one does
# the same job for macOS, and the two differ in three ways that are worth
# stating rather than diffing for:
#
#   * Everything here is static. A loose dylib beside a core is not something
#     iOS or tvOS will load, so nothing may end up in the core's NEEDED list
#     that is not part of the system.
#   * Everything here is a cross-build, even on an Apple Silicon host: same
#     instruction set, different SDK. CMAKE_SYSTEM_NAME is what tells CMake
#     that, and it changes enough behaviour that the flags below are spelled
#     out one by one with the reason attached.
#   * Qt, ffmpeg, harfbuzz, MoltenVK and shaderc are not built at all. The core
#     links no UI and no media, and these two platforms have no Vulkan to
#     compile shaders for - they render through Metal.
#
# Versions and checksums are kept in step with build-dependencies-slice.sh; if
# you bump one there, bump it here too.

set -e

if [ "$#" -ne 1 ]; then
	echo "Syntax: $0 <output directory>" >&2
	exit 1
fi

# Which Apple embedded platform. The recipes are identical - same sources, same
# options - so this only picks the SDK and the system name handed to every
# sub-build.
: ${APPLE_PLATFORM:=ios}

case "$APPLE_PLATFORM" in
	ios)
		SYSTEM_NAME=iOS
		SYSROOT=iphoneos
		;;
	tvos)
		SYSTEM_NAME=tvOS
		SYSROOT=appletvos
		;;
	*)
		echo "unknown APPLE_PLATFORM '$APPLE_PLATFORM' (expected ios or tvos)" >&2
		exit 1
		;;
esac

NPROCS="$(getconf _NPROCESSORS_ONLN)"
INSTALLDIR="$1"
if [ "${INSTALLDIR:0:1}" != "/" ]; then
	INSTALLDIR="$PWD/$INSTALLDIR"
fi

# The build passes this in: the core's own job sets one deployment target and
# hands it here, so the dependencies and the core are built against the same
# one. The default is for running this script by hand - 17.4 matches the iOS app
# in platforms/ios, and tvOS takes the same number, which is a real tvOS release
# too.
: ${APPLE_DEPLOYMENT_TARGET:=17.4}

FREETYPE=2.14.3
SDL=SDL3-3.4.12
ZSTD=1.5.7
LZ4=1.10.0
LIBPNG=1.6.58
LIBJPEGTURBO=3.2.0
LIBWEBP=1.6.0
PLUTOVG=1.3.2
PLUTOSVG=0.0.7

mkdir -p deps-build
cd deps-build

# The flags every sub-build gets, and why each one is here:
#
#   CMAKE_SYSTEM_PROCESSOR - naming CMAKE_SYSTEM_NAME puts CMake into
#     cross-compiling mode, and in that mode it stops detecting the processor
#     and leaves the variable empty unless told. That is not a warning but a
#     hard failure a couple of libraries down: libjpeg-turbo does
#     string(TOLOWER ${CMAKE_SYSTEM_PROCESSOR} ...) and gets "string no output
#     variable specified" from the empty expansion.
#
#   CMAKE_FIND_ROOT_PATH - Platform/Darwin.cmake sets the find-root-path modes
#     to ONLY for the embedded systems, so CMAKE_PREFIX_PATH is re-rooted under
#     the find roots rather than searched as it stands, and a library looking
#     for one installed here a moment ago misses it and searches only the SDK.
#     (FreeType failing with "Could NOT find PNG" three libraries after libpng
#     was installed into this very prefix.) Darwin.cmake appends the sysroot to
#     whatever it is given rather than replacing it, so naming the prefix here
#     searches both - and the modes stay ONLY, which is what keeps a macOS
#     library out of an iOS build.
#
#   CMAKE_MACOSX_BUNDLE=OFF - Darwin.cmake turns bundles on by default for the
#     embedded systems, which makes every executable a bundle, and an
#     install(TARGETS) that does not name a BUNDLE DESTINATION is then a hard
#     error. Nothing here is an application.
CMAKE_COMMON=(
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_SYSTEM_NAME="$SYSTEM_NAME"
	-DCMAKE_OSX_SYSROOT="$SYSROOT"
	-DCMAKE_OSX_ARCHITECTURES=arm64
	-DCMAKE_SYSTEM_PROCESSOR=arm64
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$APPLE_DEPLOYMENT_TARGET"
	-DCMAKE_PREFIX_PATH="$INSTALLDIR"
	-DCMAKE_FIND_ROOT_PATH="$INSTALLDIR"
	-DCMAKE_INSTALL_PREFIX="$INSTALLDIR"
	-DCMAKE_MACOSX_BUNDLE=OFF
	-DBUILD_SHARED_LIBS=OFF
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5
)

grep . > SHASUMS <<EOF
36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f  freetype-$FREETYPE.tar.xz
f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7  $SDL.tar.gz
eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3  zstd-$ZSTD.tar.gz
537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b  lz4-$LZ4.tar.gz
28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775  libpng-$LIBPNG.tar.xz
e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564  libwebp-$LIBWEBP.tar.gz
6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e  libjpeg-turbo-$LIBJPEGTURBO.tar.gz
7bd4e79ce18b1d47517e7e91fbb7cf19d4f01942804a519bc7c0bf32b6325dd5  plutovg-$PLUTOVG.tar.gz
78561b571ac224030cdc450ca2986b4de915c2ba7616004a6d71a379bffd15f3  plutosvg-$PLUTOSVG.tar.gz
EOF

if ! shasum -sa 256 --check SHASUMS 2> /dev/null; then
	curl -L \
		-O "https://sourceforge.net/projects/freetype/files/freetype2/$FREETYPE/freetype-$FREETYPE.tar.xz" \
		-O "https://libsdl.org/release/$SDL.tar.gz" \
		-O "https://github.com/facebook/zstd/releases/download/v$ZSTD/zstd-$ZSTD.tar.gz" \
		-O "https://github.com/lz4/lz4/releases/download/v$LZ4/lz4-$LZ4.tar.gz" \
		-O "https://downloads.sourceforge.net/project/libpng/libpng16/$LIBPNG/libpng-$LIBPNG.tar.xz" \
		-O "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LIBJPEGTURBO/libjpeg-turbo-$LIBJPEGTURBO.tar.gz" \
		-O "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$LIBWEBP.tar.gz" \
		-O "https://github.com/sammycage/plutovg/archive/v$PLUTOVG/plutovg-$PLUTOVG.tar.gz" \
		-O "https://github.com/sammycage/plutosvg/archive/v$PLUTOSVG/plutosvg-$PLUTOSVG.tar.gz"
fi

shasum -a 256 --check --strict SHASUMS

echo "Installing libpng..."
rm -fr "libpng-$LIBPNG"
tar xf "libpng-$LIBPNG.tar.xz"
cd "libpng-$LIBPNG"
cmake "${CMAKE_COMMON[@]}" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
	-DPNG_FRAMEWORK=OFF -B build
make -C build "-j$NPROCS"
make -C build install
cd ..

echo "Installing libjpeg-turbo..."
rm -fr "libjpeg-turbo-$LIBJPEGTURBO"
tar xf "libjpeg-turbo-$LIBJPEGTURBO.tar.gz"
cd "libjpeg-turbo-$LIBJPEGTURBO"
# WITH_SIMD=OFF: the NEON paths assemble through the host's assembler settings
# and are not worth a cross-build argument for a library the core uses to load
# a handful of images.
cmake "${CMAKE_COMMON[@]}" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF \
	-DWITH_TURBOJPEG=OFF -B build
make -C build "-j$NPROCS"
make -C build install
cd ..

echo "Installing libwebp..."
rm -fr "libwebp-$LIBWEBP"
tar xf "libwebp-$LIBWEBP.tar.gz"
cd "libwebp-$LIBWEBP"
cmake "${CMAKE_COMMON[@]}" -B build \
	-DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
	-DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
	-DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
make -C build "-j$NPROCS"
make -C build install
cd ..
# sharpyuv is a separate archive in a static build, and the module-style
# find_package(WebP) the tree uses names only libwebp - so fold one into the
# other rather than teaching the find module about a library that does not
# exist in a shared build.
libtool -static -o "$INSTALLDIR/lib/libwebp_merged.a" "$INSTALLDIR/lib/libwebp.a" "$INSTALLDIR/lib/libsharpyuv.a"
mv "$INSTALLDIR/lib/libwebp_merged.a" "$INSTALLDIR/lib/libwebp.a"

echo "Installing zstd..."
rm -fr "zstd-$ZSTD"
tar xf "zstd-$ZSTD.tar.gz"
cd "zstd-$ZSTD"
cmake "${CMAKE_COMMON[@]}" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
	-DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -B build-dir build/cmake
make -C build-dir "-j$NPROCS"
make -C build-dir install
cd ..

echo "Installing lz4..."
rm -fr "lz4-$LZ4"
tar xf "lz4-$LZ4.tar.gz"
cd "lz4-$LZ4"
cmake "${CMAKE_COMMON[@]}" -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -B build-dir build/cmake
make -C build-dir "-j$NPROCS"
make -C build-dir install
cd ..

echo "Installing freetype..."
rm -fr "freetype-$FREETYPE"
tar xf "freetype-$FREETYPE.tar.xz"
cd "freetype-$FREETYPE"
# PNG is required rather than optional: COLRv0 emoji come out of it, and the
# emoji font is one of the things the core's ImGui layer draws.
cmake "${CMAKE_COMMON[@]}" -DFT_REQUIRE_PNG=TRUE -DFT_REQUIRE_ZLIB=TRUE \
	-DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -B build
make -C build "-j$NPROCS"
make -C build install
cd ..

echo "Installing SDL..."
rm -fr "$SDL"
tar xf "$SDL.tar.gz"
cd "$SDL"
# The core links SDL for its controller support only. Video stays in on these
# platforms, unlike the macOS slice: SDL's UIKit joystick backend calls
# SDL_IsAppleTV and SDL_IsIPad, which are defined in the UIKit *video* sources,
# so a video-less build links a controller backend whose two helpers do not
# exist. Nothing here opens a window either way.
cmake "${CMAKE_COMMON[@]}" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_POWER=OFF \
	-DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -B build
make -C build "-j$NPROCS"
make -C build install
cd ..

echo "Installing plutovg..."
rm -fr "plutovg-$PLUTOVG"
tar xf "plutovg-$PLUTOVG.tar.gz"
cd "plutovg-$PLUTOVG"
cmake "${CMAKE_COMMON[@]}" -DPLUTOVG_BUILD_EXAMPLES=OFF -B build
make -C build "-j$NPROCS"
make -C build install
cd ..

echo "Installing plutosvg..."
rm -fr "plutosvg-$PLUTOSVG"
tar xf "plutosvg-$PLUTOSVG.tar.gz"
cd "plutosvg-$PLUTOSVG"
cmake "${CMAKE_COMMON[@]}" -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF -B build
make -C build "-j$NPROCS"
make -C build install
cd ..

# No libpcap. DEV9 compiles its pcap backend out on these platforms (pcsx2's
# CMakeLists excludes pcap_io.cpp and takes the Android stub instead when IOS is
# set, which the buildbot's templates set for tvOS as well), so there is nothing
# here to find it with.

echo "Dependencies for $APPLE_PLATFORM installed into $INSTALLDIR"
