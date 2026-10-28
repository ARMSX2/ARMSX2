#!/usr/bin/env bash
set -euo pipefail

# Configures and builds the PCSX2 core for one platform with Ninja into build/core-<platform>.
# Usage: build-ios-core.sh <sim|device|catalyst> [--configure]

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLATFORM="${1:-}"
MODE="${2:-}"

usage() {
	echo "usage: build-ios-core.sh <sim|device|catalyst> [--configure]" >&2
	exit 2
}

if [ $# -gt 2 ] || { [ -n "$MODE" ] && [ "$MODE" != --configure ]; }; then
	usage
fi

case "$PLATFORM" in
	sim)
		SDK=iphonesimulator
		FLAGS=(-DARMSX2_REAL_DEVICE=OFF -DARMSX2_MAC_CATALYST=OFF)
		;;
	device)
		SDK=iphoneos
		FLAGS=(-DARMSX2_REAL_DEVICE=ON -DARMSX2_MAC_CATALYST=OFF)
		;;
	catalyst)
		SDK=macosx
		M="$(xcrun --sdk macosx --show-sdk-path)"
		SUPPORT="-isystem $M/System/iOSSupport/usr/include -iframework $M/System/iOSSupport/System/Library/Frameworks -iframework $M/System/iOSSupport/System/Library/SubFrameworks"
		FLAGS=(-DARMSX2_MAC_CATALYST=ON -DARMSX2_REAL_DEVICE=OFF "-DCMAKE_C_FLAGS=$SUPPORT" "-DCMAKE_CXX_FLAGS=$SUPPORT")
		;;
	*) usage ;;
esac

# Xcode runs build scripts with a minimal PATH, so the app passes absolute tool paths in.
CMAKE="${ARMSX2_CMAKE:-cmake}"
NINJA="${ARMSX2_NINJA:-ninja}"
if ! command -v "$CMAKE" >/dev/null 2>&1; then
	echo "error: cmake is required to build the iOS core." >&2
	exit 1
fi
if ! command -v "$NINJA" >/dev/null 2>&1; then
	echo "error: ninja is required to build the iOS core." >&2
	echo "Install it with: brew install ninja" >&2
	exit 1
fi
NINJA="$(command -v "$NINJA")"

# One builder per core dir: Xcode, the scripts and index runs can all start it at once.
if [ -z "${ARMSX2_CORE_LOCKED:-}" ]; then
	mkdir -p "$ROOT_DIR/build"
	ARMSX2_CORE_LOCKED=1 exec /usr/bin/lockf -k "$ROOT_DIR/build/core-$PLATFORM.lock" "$0" "$@"
fi

CORE="$ROOT_DIR/build/core-$PLATFORM"

# Objects from two compilers, or a build.ninja naming a removed SDK, must not survive an Xcode update.
STAMP="$(xcrun --sdk "$SDK" --show-sdk-path; xcrun clang --version | sed -n 1p)"
if [ -f "$CORE/toolchain.stamp" ] && [ "$(cat "$CORE/toolchain.stamp")" != "$STAMP" ]; then
	echo "toolchain changed; rebuilding build/core-$PLATFORM from scratch"
	rm -rf "$CORE"
fi

ARGS=(
	-DCMAKE_MAKE_PROGRAM="$NINJA"
	-DCMAKE_SYSTEM_NAME=iOS
	-DCMAKE_OSX_ARCHITECTURES=arm64
	-DCMAKE_BUILD_TYPE=Release
	-DARMSX2_IOS_DSYM=ON
	-DARMSX2_CORE_ONLY=ON
	-DLTO_PCSX2_CORE=OFF
	"${FLAGS[@]}"
)
# Ninja's own re-run of CMake keeps the cached values, so a changed flag here needs a configure.
ARGS_STAMP="$(printf '%s\n' "${ARGS[@]}")"
FRESH=
# A -D dropped from ARGS would otherwise keep its old value in CMakeCache.txt.
if [ -f "$CORE/configure.stamp" ] && [ "$(cat "$CORE/configure.stamp")" != "$ARGS_STAMP" ]; then
	FRESH=1
fi
if [ "$MODE" = --configure ] || [ ! -f "$CORE/build.ninja" ] \
	|| [ "$(cat "$CORE/configure.stamp" 2>/dev/null)" != "$ARGS_STAMP" ]; then
	"$CMAKE" ${FRESH:+--fresh} -S "$ROOT_DIR" -B "$CORE" -G Ninja "${ARGS[@]}"
	printf '%s\n' "$STAMP" > "$CORE/toolchain.stamp"
	printf '%s\n' "$ARGS_STAMP" > "$CORE/configure.stamp"
fi

if [ "$MODE" != --configure ]; then
	"$CMAKE" --build "$CORE" --target armsx2_core_libs
	# A configure inside Xcode can lose Rust and turn librashader off under an app that still links it.
	if [ -n "${ARMSX2_APP_LIBRASHADER:-}" ] \
		&& ! grep -qx "set(ARMSX2_HAVE_LIBRASHADER \"$ARMSX2_APP_LIBRASHADER\")" "$CORE/armsx2-core.cmake"; then
		echo "error: librashader in build/core-$PLATFORM no longer matches the app project." >&2
		echo "Re-run the generate script from a shell where Rust is set up." >&2
		exit 1
	fi
fi
