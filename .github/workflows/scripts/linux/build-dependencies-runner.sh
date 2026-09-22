#!/usr/bin/env bash

set -e

if [ "$#" -ne 1 ]; then
    echo "Syntax: $0 <output directory>"
    exit 1
fi

SCRIPTDIR=$(realpath $(dirname "${BASH_SOURCE[0]}"))
NPROCS="$(getconf _NPROCESSORS_ONLN)"
INSTALLDIR="$1"
if [ "${INSTALLDIR:0:1}" != "/" ]; then
        INSTALLDIR="$PWD/$INSTALLDIR"
fi

# The libretro core ships as a bare armsx2_libretro.so with nothing beside it -
# no lib/ directory of its own to fall back on, unlike the SDL handheld build
# that also uses this prefix and bundles whatever ldd finds. So it needs every
# dependency folded into the one file: static, and position-independent since
# it all ends up inside a shared object. Default is today's behaviour, shared
# libraries, unchanged for every existing caller.
: ${ARMSX2_DEPS_STATIC:=0}

# Normalised before use: a bare `-ne 0` test on ARMSX2_DEPS_STATIC=true is an
# error, and an erroring test in an if reads as false, so asking for static the
# obvious way would have quietly built shared.
case "$ARMSX2_DEPS_STATIC" in
	0|""|[Nn][Oo]|[Ff][Aa][Ll][Ss][Ee]|[Oo][Ff][Ff]) DEPS_STATIC=0 ;;
	*) DEPS_STATIC=1 ;;
esac

if [ "$DEPS_STATIC" -ne 0 ]; then
	SHARED_LIBS=OFF
	STATIC_LIBS=ON
	PIC_FLAG="-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
	LIBBACKTRACE_PIC_FLAG="--with-pic"
	SHADERC_PATCH="shaderc-changes-static.patch"
	# FreeType 2.14 added FT_DYNAMIC_HARFBUZZ, it defaults ON, and it quietly
	# overrides the FT_REQUIRE_HARFBUZZ below: FreeType dlopens HarfBuzz rather
	# than linking it. Static mode installs no libharfbuzz.so for that dlopen to
	# find, so it would fail on the device and autofit's HarfBuzz-assisted
	# hinting would just go away - no error, only worse-looking text. Off, so
	# the second FreeType pass links HarfBuzz for real and the build of it above
	# is not wasted.
	FT_HARFBUZZ_FLAG="-DFT_DYNAMIC_HARFBUZZ=OFF"
else
	SHARED_LIBS=ON
	STATIC_LIBS=OFF
	PIC_FLAG=
	LIBBACKTRACE_PIC_FLAG=
	SHADERC_PATCH="shaderc-changes.patch"
	FT_HARFBUZZ_FLAG=
fi

FREETYPE=2.14.1
HARFBUZZ=12.2.0
LIBBACKTRACE=ad106d5fdd5d960bd33fae1c48a351af567fd075
LIBPNG=1.6.51
LIBWEBP=1.6.0
SDL=SDL3-3.2.26
LZ4=1.10.0
ZSTD=1.5.7
PLUTOVG=1.3.2
PLUTOSVG=0.0.7
RAPIDYAML=0.12.1

# Static mode only - see the ARMSX2_DEPS_STATIC branch near the bottom. In
# shared mode these two come from the CI image's apt packages instead, same as
# always, so their versions here don't have to track apt's.
JPEGTURBO=3.2.0
LIBPCAP=libpcap-1.10.5

SHADERC=2025.4
SHADERC_GLSLANG=7a47e2531cb334982b2a2dd8513dca0a3de4373d
SHADERC_SPIRVHEADERS=b824a462d4256d720bebb40e78b9eb8f78bbb305
SHADERC_SPIRVTOOLS=971a7b6e8d7740035bbff089bbbf9f42951ecfd5

mkdir -p deps-build
cd deps-build

grep . > SHASUMS <<EOF
32427e8c471ac095853212a37aef816c60b42052d4d9e48230bab3bdf2936ccc  freetype-$FREETYPE.tar.xz
f63fc519f150465bd0bdafcdf3d0e9c23474f4c474171cd515ea1b3a72c081fb  harfbuzz-$HARFBUZZ.tar.gz
96e5c2d7f2c482a60d5804da48a2eb9a0db0719b2c65dcc169fbfdcf37f3a45d  libbacktrace-$LIBBACKTRACE.tar.gz
a050a892d3b4a7bb010c3a95c7301e49656d72a64f1fc709a90b8aded192bed2  libpng-$LIBPNG.tar.xz
e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564  libwebp-$LIBWEBP.tar.gz
dad488474a51a0b01d547cd2834893d6299328d2e30f479a3564088b5476bae2  $SDL.tar.gz
9c16ec5654be709f062a705d0c6f529193f1c2123fe7f102fda6733913689023  libpng-$LIBPNG-apng.patch.gz
537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b  lz4-$LZ4.tar.gz
eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3  zstd-$ZSTD.tar.gz
8a89fb6612ace8954470aae004623374a8fc8b7a34a4277bee5527173b064faf  shaderc-$SHADERC.tar.gz
272d2725b140e09e85b96eecdc59c2e00c1a14cda2301767e1bf3c363a44b931  shaderc-glslang-$SHADERC_GLSLANG.tar.gz
c693867f10a7760ef1bcf85419d51783586768cc2c601d03841bc6a8b2554b9c  shaderc-spirv-headers-$SHADERC_SPIRVHEADERS.tar.gz
06b0a042f2e121e954badb4fd78c9e2d4bc7ed6087eceb26ab559c23cf94334f  shaderc-spirv-tools-$SHADERC_SPIRVTOOLS.tar.gz
7bd4e79ce18b1d47517e7e91fbb7cf19d4f01942804a519bc7c0bf32b6325dd5  plutovg-$PLUTOVG.tar.gz
78561b571ac224030cdc450ca2986b4de915c2ba7616004a6d71a379bffd15f3  plutosvg-$PLUTOSVG.tar.gz
e9efcdd17f86287748793cf21d106e461fcad8d103a3e5a23632afe93828660d  rapidyaml-$RAPIDYAML-src.tgz
EOF

if [ "$DEPS_STATIC" -ne 0 ]; then
	grep . >> SHASUMS <<EOF
6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e  libjpeg-turbo-$JPEGTURBO.tar.gz
6cd9835338ca334b699b1217e2aee2b873463c76aafd19b8b9d4710554f025ac  $LIBPCAP.tar.gz
EOF
fi

if ! shasum -sa 256 --check SHASUMS 2> /dev/null; then
	curl -L \
		-O "https://sourceforge.net/projects/freetype/files/freetype2/$FREETYPE/freetype-$FREETYPE.tar.xz" \
		-O "https://github.com/harfbuzz/harfbuzz/archive/$HARFBUZZ/harfbuzz-$HARFBUZZ.tar.gz" \
		-O "https://github.com/ianlancetaylor/libbacktrace/archive/$LIBBACKTRACE/libbacktrace-$LIBBACKTRACE.tar.gz" \
		-O "https://downloads.sourceforge.net/project/libpng/libpng16/$LIBPNG/libpng-$LIBPNG.tar.xz" \
		-O "https://download.sourceforge.net/libpng-apng/libpng-$LIBPNG-apng.patch.gz" \
		-O "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$LIBWEBP.tar.gz" \
		-O "https://github.com/lz4/lz4/releases/download/v$LZ4/lz4-$LZ4.tar.gz" \
		-O "https://libsdl.org/release/$SDL.tar.gz" \
		-O "https://github.com/facebook/zstd/releases/download/v$ZSTD/zstd-$ZSTD.tar.gz" \
		-O "https://github.com/google/shaderc/archive/v$SHADERC/shaderc-$SHADERC.tar.gz" \
		-O "https://github.com/KhronosGroup/glslang/archive/$SHADERC_GLSLANG/shaderc-glslang-$SHADERC_GLSLANG.tar.gz" \
		-O "https://github.com/KhronosGroup/SPIRV-Headers/archive/$SHADERC_SPIRVHEADERS/shaderc-spirv-headers-$SHADERC_SPIRVHEADERS.tar.gz" \
		-O "https://github.com/KhronosGroup/SPIRV-Tools/archive/$SHADERC_SPIRVTOOLS/shaderc-spirv-tools-$SHADERC_SPIRVTOOLS.tar.gz" \
		-O "https://github.com/sammycage/plutovg/archive/v$PLUTOVG/plutovg-$PLUTOVG.tar.gz" \
		-O "https://github.com/sammycage/plutosvg/archive/v$PLUTOSVG/plutosvg-$PLUTOSVG.tar.gz" \
		-O "https://github.com/biojppm/rapidyaml/releases/download/v$RAPIDYAML/rapidyaml-$RAPIDYAML-src.tgz"
	if [ "$DEPS_STATIC" -ne 0 ]; then
		curl -L \
			-O "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JPEGTURBO/libjpeg-turbo-$JPEGTURBO.tar.gz" \
			-O "https://github.com/the-tcpdump-group/libpcap/archive/$LIBPCAP/$LIBPCAP.tar.gz"
	fi
fi

shasum -a 256 --check --strict SHASUMS

echo "Building libbacktrace..."
rm -fr "libbacktrace-$LIBBACKTRACE"
tar xf "libbacktrace-$LIBBACKTRACE.tar.gz"
cd "libbacktrace-$LIBBACKTRACE"
./configure --prefix="$INSTALLDIR" $LIBBACKTRACE_PIC_FLAG
make
make install
cd ..

echo "Building libpng..."
rm -fr "libpng-$LIBPNG"
tar xf "libpng-$LIBPNG.tar.xz"
gzip -kd -f "libpng-$LIBPNG-apng.patch.gz"
cd "libpng-$LIBPNG"
patch -p1 < "../libpng-$LIBPNG-apng.patch"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -DPNG_TESTS=OFF -DPNG_STATIC=$STATIC_LIBS -DPNG_SHARED=$SHARED_LIBS -DPNG_TOOLS=OFF -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building LZ4..."
rm -fr "lz4-$LZ4"
tar xf "lz4-$LZ4.tar.gz"
cd "lz4-$LZ4"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -DBUILD_STATIC_LIBS=$STATIC_LIBS -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -B build-dir -G Ninja build/cmake
cmake --build build-dir --parallel
ninja -C build-dir install
cd ..

echo "Building Zstandard..."
rm -fr "zstd-$ZSTD"
tar xf "zstd-$ZSTD.tar.gz"
cd "zstd-$ZSTD"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -DZSTD_BUILD_SHARED=$SHARED_LIBS -DZSTD_BUILD_STATIC=$STATIC_LIBS -DZSTD_BUILD_PROGRAMS=OFF -B build -G Ninja build/cmake
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building FreeType without HarfBuzz..."
rm -fr "freetype-$FREETYPE"
tar xf "freetype-$FREETYPE.tar.xz"
cd "freetype-$FREETYPE"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building HarfBuzz..."
rm -fr "harfbuzz-$HARFBUZZ"
tar xf "harfbuzz-$HARFBUZZ.tar.gz"
cd "harfbuzz-$HARFBUZZ"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -DHB_BUILD_UTILS=OFF -DHB_HAVE_FREETYPE=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building WebP..."
rm -fr "libwebp-$LIBWEBP"
tar xf "libwebp-$LIBWEBP.tar.gz"
cd "libwebp-$LIBWEBP"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -B build -G Ninja \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
  -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=$SHARED_LIBS
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building FreeType with HarfBuzz..."
rm -fr "freetype-$FREETYPE"
tar xf "freetype-$FREETYPE.tar.xz"
cd "freetype-$FREETYPE"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_REQUIRE_HARFBUZZ=TRUE $FT_HARFBUZZ_FLAG -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building SDL..."
rm -fr "$SDL"
tar xf "$SDL.tar.gz"
cd "$SDL"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -DSDL_SHARED=$SHARED_LIBS -DSDL_STATIC=$STATIC_LIBS -DSDL_VIDEO=OFF -DSDL_POWER=OFF -DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_UNIX_CONSOLE_BUILD=ON -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building PlutoVG..."
rm -fr "plutovg-$PLUTOVG"
tar xf "plutovg-$PLUTOVG.tar.gz"
cd "plutovg-$PLUTOVG"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -DPLUTOVG_BUILD_EXAMPLES=OFF -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building PlutoSVG..."
rm -fr "plutosvg-$PLUTOSVG"
tar xf "plutosvg-$PLUTOSVG.tar.gz"
cd "plutosvg-$PLUTOSVG"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building RapidYAML..."
rm -fr "rapidyaml-$RAPIDYAML-src"
tar xf "rapidyaml-$RAPIDYAML-src.tgz"
cd "rapidyaml-$RAPIDYAML-src"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DBUILD_SHARED_LIBS=$SHARED_LIBS -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building shaderc..."
rm -fr "shaderc-$SHADERC"
tar xf "shaderc-$SHADERC.tar.gz"
cd "shaderc-$SHADERC"
cd third_party
tar xf "../../shaderc-glslang-$SHADERC_GLSLANG.tar.gz"
mv "glslang-$SHADERC_GLSLANG" "glslang"
tar xf "../../shaderc-spirv-headers-$SHADERC_SPIRVHEADERS.tar.gz"
mv "SPIRV-Headers-$SHADERC_SPIRVHEADERS" "spirv-headers"
tar xf "../../shaderc-spirv-tools-$SHADERC_SPIRVTOOLS.tar.gz"
mv "SPIRV-Tools-$SHADERC_SPIRVTOOLS" "spirv-tools"
cd ..
patch -p1 < "$SCRIPTDIR/../common/$SHADERC_PATCH"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" $PIC_FLAG -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

if [ "$DEPS_STATIC" -ne 0 ]; then
	# Shared mode takes these two from the CI image's apt packages
	# (libjpeg-dev, libpcap-dev), which is fine for a build that isn't
	# travelling anywhere apt didn't also put them. The libretro core does
	# travel, so here they're built from source and pinned like everything
	# else in this script.
	echo "Building libjpeg-turbo..."
	rm -fr "libjpeg-turbo-$JPEGTURBO"
	tar xf "libjpeg-turbo-$JPEGTURBO.tar.gz"
	cd "libjpeg-turbo-$JPEGTURBO"
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -B build -G Ninja
	cmake --build build --parallel
	ninja -C build install
	cd ..

	echo "Building libpcap..."
	rm -fr "libpcap-$LIBPCAP"
	tar xf "$LIBPCAP.tar.gz"
	cd "libpcap-$LIBPCAP"
	# DEV9 links libpcap directly, so an archive with dangling references to
	# D-Bus or libnl breaks cmake/FindPCAP.cmake's own solo-link check (it
	# tests -lpcap alone, then -lpcap plus pthread, and FATAL_ERRORs if
	# neither works) - so every optional backend that could pull in a symbol
	# from outside libpcap itself is off.
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DDISABLE_DBUS=ON -DDISABLE_RDMA=ON -DDISABLE_DAG=ON -DDISABLE_SEPTEL=ON -DDISABLE_SNF=ON -DDISABLE_TC=ON -DDISABLE_NETMAP=ON -DDISABLE_DPDK=ON -DDISABLE_BLUETOOTH=ON -DDISABLE_LINUX_USBMON=ON -DBUILD_WITH_LIBNL=OFF -DENABLE_REMOTE=OFF -B build -G Ninja
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

echo "Cleaning up..."
cd ..
rm -rf deps-build
