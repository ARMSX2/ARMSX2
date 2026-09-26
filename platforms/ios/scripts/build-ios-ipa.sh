#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-ios-xcode}"
PROJECT="$BUILD_DIR/ARMSX2iOS.xcodeproj"
IPA_NAME="${IPA_NAME:-ARMSX2-iOS-unsigned.ipa}"
APP_PATH="$BUILD_DIR/Release-iphoneos/ARMSX2iOS.app"
STAGING_DIR="$BUILD_DIR/ipa-staging"
BUILD_LOG="$BUILD_DIR/xcodebuild.log"
ENTITLEMENTS_FILE="${ENTITLEMENTS_FILE:-$ROOT_DIR/app/src/main/cpp/Entitlements.plist}"
SIGN_IDENTITY="${SIGN_IDENTITY:-}"
AD_HOC_SIGN="${AD_HOC_SIGN:-0}"
RUSTUP_HOME="${RUSTUP_HOME:-$BUILD_DIR/rustup-home}"
CARGO_HOME="${CARGO_HOME:-$BUILD_DIR/cargo-home}"
LIBRASHADER_RUST_TARGET="aarch64-apple-ios"

require_tool() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "error: required tool '$1' was not found." >&2
		exit 1
	fi
}

ensure_librashader_toolchain() {
	export RUSTUP_HOME CARGO_HOME
	if [[ -x "$CARGO_HOME/bin/cargo" ]]; then
		export PATH="$CARGO_HOME/bin:$PATH"
	fi

	if ! command -v cargo >/dev/null 2>&1 \
		|| ! command -v rustc >/dev/null 2>&1; then
		require_tool curl
		local rustup_platform rustup_url installer expected_hash actual_hash
		case "$(uname -m)" in
			arm64) rustup_platform="aarch64-apple-darwin" ;;
			x86_64) rustup_platform="x86_64-apple-darwin" ;;
			*) echo "error: unsupported Rust host architecture: $(uname -m)" >&2; exit 1 ;;
		esac
		rustup_url="https://static.rust-lang.org/rustup/dist/$rustup_platform/rustup-init"
		installer="$BUILD_DIR/rustup-init"
		echo "Installing the project-local Rust toolchain required by librashader..."
		curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
			"$rustup_url" --output "$installer"
		expected_hash="$(curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
			"$rustup_url.sha256" | awk '{print $1}')"
		actual_hash="$(shasum -a 256 "$installer" | awk '{print $1}')"
		if [[ -z "$expected_hash" || "$actual_hash" != "$expected_hash" ]]; then
			echo "error: rustup-init checksum verification failed." >&2
			exit 1
		fi
		chmod u+x "$installer"
		"$installer" -y --no-modify-path --profile minimal --default-toolchain stable
		export PATH="$CARGO_HOME/bin:$PATH"
	fi

	if command -v rustup >/dev/null 2>&1; then
		rustup target add "$LIBRASHADER_RUST_TARGET"
	fi

	local target_libdir
	target_libdir="$(rustc --print target-libdir --target "$LIBRASHADER_RUST_TARGET" 2>/dev/null || true)"
	if [[ -z "$target_libdir" || ! -d "$target_libdir" ]]; then
		echo "error: Rust target $LIBRASHADER_RUST_TARGET is required for the production iOS shader chain." >&2
		exit 1
	fi
}

refresh_generated_git_metadata() {
	local short_hash full_hash git_date pbxproj svnrev_file
	short_hash="$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || true)"
	full_hash="$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || true)"
	git_date="$(git -C "$ROOT_DIR" log -1 --format=%cd --date=local 2>/dev/null || true)"
	[[ -n "$short_hash" ]] || return 0

	pbxproj="$PROJECT/project.pbxproj"
	if [[ -f "$pbxproj" ]]; then
		SHORT_HASH="$short_hash" perl -0pi -e 's/ARMSX2_GIT_HASH=\\"[0-9A-Fa-f]+\\"/ARMSX2_GIT_HASH=\\"$ENV{SHORT_HASH}\\"/g' "$pbxproj"
	fi

	svnrev_file="$BUILD_DIR/common/include/svnrev.h"
	if [[ -f "$svnrev_file" ]]; then
		SHORT_HASH="$short_hash" FULL_HASH="$full_hash" GIT_DATE_TEXT="$git_date" perl -0pi -e '
			s/(#define GIT_REV "[^"]*-g)[0-9A-Fa-f]+(")/$1$ENV{SHORT_HASH}$2/g;
			s/(#define GIT_HASH ")[^"]*(")/$1$ENV{FULL_HASH}$2/g;
			s/(#define GIT_DATE ")[^"]*(")/$1$ENV{GIT_DATE_TEXT}$2/g;
		' "$svnrev_file"
	fi
}

mkdir -p "$BUILD_DIR"
ensure_librashader_toolchain

if ! command -v xcodebuild >/dev/null 2>&1; then
	echo "error: xcodebuild was not found. Install full Xcode from Apple, then run:" >&2
	echo "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
	exit 1
fi

if ! xcrun --sdk iphoneos --find metal >/dev/null 2>&1 || ! xcrun --sdk iphoneos --find metallib >/dev/null 2>&1; then
	echo "error: the iPhoneOS Metal compiler tools were not found." >&2
	echo "Make sure full Xcode is selected, not Command Line Tools only:" >&2
	echo "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
	echo "  xcodebuild -downloadPlatform iOS" >&2
	exit 1
fi

echo "Using Xcode:"
xcodebuild -version
echo "Developer directory: $(xcode-select -p)"
echo "Metal compiler: $(xcrun --sdk iphoneos --find metal)"
echo

if command -v cmake >/dev/null 2>&1; then
	"$ROOT_DIR/scripts/generate-ios-xcode.sh"
elif [[ ! -d "$PROJECT" ]]; then
	echo "error: cmake is required to generate the iOS Xcode project." >&2
	echo "Install CMake from https://cmake.org/download/ or through your package manager." >&2
	exit 1
else
	echo "cmake not found; reusing existing generated Xcode project."
fi
if ! grep -q "ARMSX2_HAS_LIBRASHADER" "$PROJECT/project.pbxproj"; then
	echo "error: production project omitted librashader; refusing to package a shader-disabled IPA." >&2
	exit 1
fi
refresh_generated_git_metadata

set +e
xcodebuild \
	-project "$PROJECT" \
	-scheme ARMSX2iOS \
	-configuration Release \
	-sdk iphoneos \
	CODE_SIGNING_ALLOWED=NO \
	CODE_SIGNING_REQUIRED=NO \
	CODE_SIGN_IDENTITY="" \
	build 2>&1 | tee "$BUILD_LOG"
XCODEBUILD_STATUS=${PIPESTATUS[0]}
set -e

if [[ "$XCODEBUILD_STATUS" -ne 0 ]]; then
	if grep -q "CompileMetalFile" "$BUILD_LOG"; then
		echo >&2
		echo "Metal shader compilation failed. Common fixes:" >&2
		echo "  1. Select full Xcode: sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
		echo "  2. Install the iOS platform in Xcode Settings > Platforms, or run: xcodebuild -downloadPlatform iOS" >&2
		echo "  3. Open $BUILD_LOG and search above the CompileMetalFile line for the first shader error." >&2
	fi
	exit "$XCODEBUILD_STATUS"
fi

if [[ ! -d "$APP_PATH" ]]; then
	echo "error: built app was not found at $APP_PATH" >&2
	exit 1
fi

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR/Payload"
ditto "$APP_PATH" "$STAGING_DIR/Payload/ARMSX2iOS.app"
STAGED_APP="$STAGING_DIR/Payload/ARMSX2iOS.app"

CONTROLLER_SKINS_DIR="$ROOT_DIR/app/src/main/assets/app_icons/controller_skins"
if [[ -d "$CONTROLLER_SKINS_DIR" ]]; then
	ditto "$CONTROLLER_SKINS_DIR" "$STAGED_APP/controller_skins"
fi

if [[ -n "$SIGN_IDENTITY" || "$AD_HOC_SIGN" == "1" ]]; then
	if [[ ! -f "$ENTITLEMENTS_FILE" ]]; then
		echo "error: entitlements file was not found at $ENTITLEMENTS_FILE" >&2
		exit 1
	fi

	IDENTITY="$SIGN_IDENTITY"
	if [[ -z "$IDENTITY" ]]; then
		IDENTITY="-"
	fi

	echo "Signing staged app with identity '$IDENTITY' and entitlements:"
	echo "  $ENTITLEMENTS_FILE"

	if [[ -d "$STAGED_APP/Frameworks" ]]; then
		while IFS= read -r -d '' nested; do
			codesign --force --sign "$IDENTITY" --timestamp=none "$nested"
		done < <(find "$STAGED_APP/Frameworks" -type d -name '*.framework' -print0)
		while IFS= read -r -d '' nested; do
			codesign --force --sign "$IDENTITY" --timestamp=none "$nested"
		done < <(find "$STAGED_APP/Frameworks" -type f \( -name '*.dylib' -o -perm -111 \) -print0)
	fi

	codesign --force --sign "$IDENTITY" --entitlements "$ENTITLEMENTS_FILE" --timestamp=none "$STAGED_APP"
	codesign -d --entitlements :- "$STAGED_APP" 2>&1 | sed -n '1,80p'
fi

OUTPUT_IPA="$BUILD_DIR/$IPA_NAME"
# zip updates an existing archive in place and preserves entries that no longer
# exist in Payload. Always create the IPA from an empty output path so removed
# app resources cannot survive a later incremental package.
if [[ -e "$OUTPUT_IPA" ]]; then
	rm "$OUTPUT_IPA"
fi
(cd "$STAGING_DIR" && zip -qry "$OUTPUT_IPA" Payload)

if [[ -n "$SIGN_IDENTITY" || "$AD_HOC_SIGN" == "1" ]]; then
	echo "Created signed IPA:"
else
	echo "Created unsigned IPA:"
fi
echo "  $OUTPUT_IPA"
