#!/usr/bin/env bash
#
# Builds the dependencies of the libretro core for Android, static and PIC,
# into a local prefix. Everything comes from source: unlike the Linux jobs
# there is no distribution behind the NDK to take png, jpeg, SDL and the rest
# from, and unlike macOS there is no system Vulkan loader either.
#
# Usage: build-dependencies.sh <install-prefix>
# Environment:
#   ANDROID_NDK / ANDROID_NDK_HOME / ANDROID_NDK_ROOT - the NDK to build with
#   ANDROID_ABI  - arm64-v8a (default) or x86_64
#   ANDROID_API  - minimum platform level, default 28 (ASharedMemory is 26,
#                  bionic's posix_spawn is 28)

set -e

if [ "$#" -ne 1 ]; then
	echo "Syntax: $0 <install-prefix>"
	exit 1
fi

PREFIX=$(realpath "$1")
NPROCS="$(getconf _NPROCESSORS_ONLN)"

NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
	echo "No usable NDK: set ANDROID_NDK to one containing build/cmake/android.toolchain.cmake"
	exit 1
fi

ABI="${ANDROID_ABI:-arm64-v8a}"
API="${ANDROID_API:-28}"

# Versions follow the Linux recipe (build-dependencies-runner.sh) so the two
# cannot drift; the ones it does not build - jpeg, curl, the pcap headers - are
# system packages there and only exist here.
FREETYPE=VER-2-14-1
LIBPNG=v1.6.51
LIBWEBP=v1.6.0
SDL=release-3.2.26
LZ4=v1.10.0
ZSTD=v1.5.7
PLUTOVG=v1.3.2
PLUTOSVG=v0.0.7
JPEGTURBO=3.1.4.1
CURL=curl-8_11_1
LIBPCAP=libpcap-1.10.5
SHADERC=v2025.4
SHADERC_GLSLANG=7a47e2531cb334982b2a2dd8513dca0a3de4373d

# c++_static: several of these end up inside one .so, and a shared STL would
# have to be shipped next to the core for the frontend to find.
TOOLCHAIN=(
	"-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake"
	"-DANDROID_ABI=$ABI"
	"-DANDROID_PLATFORM=android-$API"
	"-DANDROID_STL=c++_static"
	"-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
	"-DCMAKE_INSTALL_PREFIX=$PREFIX"
	"-DCMAKE_PREFIX_PATH=$PREFIX"
	"-DCMAKE_FIND_ROOT_PATH=$PREFIX"
	"-DCMAKE_BUILD_TYPE=Release"
	"-DBUILD_SHARED_LIBS=OFF"
)

mkdir -p deps-build
cd deps-build

clone() {
	# clone <url> <dir> <tag>
	[ -d "$2" ] || git clone --depth 1 --branch "$3" --recursive "$1" "$2"
}

build() {
	# build <source-dir> <build-dir> [extra cmake args...]
	local src="$1" bld="$2"
	shift 2
	cmake -S "$src" -B "$bld" -G Ninja "${TOOLCHAIN[@]}" "$@"
	cmake --build "$bld" --parallel "$NPROCS"
	cmake --install "$bld"
}

# zlib comes with the NDK sysroot, so it is the one library not built here.

clone https://github.com/pnggroup/libpng libpng "$LIBPNG"
build libpng libpng/build -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF \
	-DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF

clone https://github.com/libjpeg-turbo/libjpeg-turbo libjpeg-turbo "$JPEGTURBO"
build libjpeg-turbo libjpeg-turbo/build -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
	-DWITH_TURBOJPEG=OFF

clone https://github.com/facebook/zstd zstd "$ZSTD"
build zstd/build/cmake zstd/b -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
	-DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF

clone https://github.com/lz4/lz4 lz4 "$LZ4"
build lz4/build/cmake lz4/b -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF

clone https://github.com/webmproject/libwebp libwebp "$LIBWEBP"
build libwebp libwebp/build -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF \
	-DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
	-DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF \
	-DWEBP_BUILD_EXTRAS=OFF

# SDL is here for the input and audio sources the emulator compiles against.
# Nothing in a libretro core opens an SDL window, so its Java side never runs.
clone https://github.com/libsdl-org/SDL sdl3 "$SDL"
build sdl3 sdl3/build -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF

clone https://github.com/freetype/freetype freetype "$FREETYPE"
build freetype freetype/build -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
	-DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON

clone https://github.com/sammycage/plutovg plutovg "$PLUTOVG"
build plutovg plutovg/build -DPLUTOVG_BUILD_EXAMPLES=OFF

clone https://github.com/sammycage/plutosvg plutosvg "$PLUTOSVG"
build plutosvg plutosvg/build -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF

# No TLS: the core does not talk to the achievement or update servers, and a
# TLS stack would drag in OpenSSL for something nothing calls.
clone https://github.com/curl/curl curl "$CURL"
build curl curl/build -DCURL_ENABLE_SSL=OFF -DBUILD_CURL_EXE=OFF \
	-DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DCURL_USE_LIBPSL=OFF \
	-DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF

# DEV9 links libpcap directly, so headers are not enough even though no Android
# device will ever hand it a capture device. Everything optional is off: the
# point is a linkable archive, not a working capture backend.
clone https://github.com/the-tcpdump-group/libpcap libpcap "$LIBPCAP"
build libpcap libpcap/build -DDISABLE_DBUS=ON -DDISABLE_RDMA=ON -DDISABLE_DAG=ON \
	-DDISABLE_SEPTEL=ON -DDISABLE_SNF=ON -DDISABLE_TC=ON -DDISABLE_NETMAP=ON \
	-DDISABLE_DPDK=ON -DDISABLE_BLUETOOTH=ON -DDISABLE_LINUX_USBMON=ON \
	-DBUILD_WITH_LIBNL=OFF -DENABLE_REMOTE=OFF

clone https://github.com/google/shaderc shaderc "$SHADERC"
(cd shaderc && python3 utils/git-sync-deps)
if [ "$(git -C shaderc/third_party/glslang rev-parse HEAD)" != "$SHADERC_GLSLANG" ]; then
	git -C shaderc/third_party/glslang fetch --depth 1 origin "$SHADERC_GLSLANG"
	git -C shaderc/third_party/glslang checkout --detach FETCH_HEAD
fi
cmake -S shaderc -B shaderc/b -G Ninja "${TOOLCHAIN[@]}" \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON
cmake --build shaderc/b --parallel "$NPROCS" --target shaderc_combined
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp shaderc/b/libshaderc/libshaderc_combined.a "$PREFIX/lib/"
cp -r shaderc/libshaderc/include/shaderc "$PREFIX/include/"

echo "Android ($ABI, API $API) dependencies installed to $PREFIX"
