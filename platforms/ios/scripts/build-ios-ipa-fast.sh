#!/usr/bin/env bash
set -euo pipefail

# Development-only IPA path. It keeps the emulator's native
# Release optimization while compiling the Swift UI with -Onone and without
# PCSX2 core LTO. Use build-ios-ipa.sh for final release artifacts.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="$ROOT_DIR/app/src/main/cpp"
SWIFT_DIR="$ROOT_DIR/app/src/main/swift"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-ios-fast-xcode}"
PROJECT="$BUILD_DIR/ARMSX2iOS.xcodeproj"
CONFIG_STAMP="$BUILD_DIR/.armsx2-fast-cmake-signature"
IPA_NAME="${IPA_NAME:-ARMSX2-iOS-development.ipa}"
APP_PATH="$BUILD_DIR/Release-iphoneos/ARMSX2iOS.app"
STAGING_DIR="$BUILD_DIR/ipa-staging"
BUILD_LOG="$BUILD_DIR/xcodebuild-development.log"
ENTITLEMENTS_FILE="${ENTITLEMENTS_FILE:-$ROOT_DIR/app/src/main/cpp/Entitlements.plist}"
BUNDLE_ID="${BUNDLE_ID:-com.armsx2.ios}"
TEAM_ID="${TEAM_ID:-}"
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
		echo "error: Rust target $LIBRASHADER_RUST_TARGET is required for the iOS shader chain." >&2
		echo "Install it with: rustup target add $LIBRASHADER_RUST_TARGET" >&2
		exit 1
	fi
}

configuration_signature() {
	{
		printf '%s\n' \
			"ARMSX2_FAST_IPA_V1" \
			"BUNDLE_ID=$BUNDLE_ID" \
			"TEAM_ID=$TEAM_ID" \
			"LTO_PCSX2_CORE=OFF" \
			"CARGO=$(command -v cargo)" \
			"RUSTC=$(rustc --version)" \
			"RUST_TARGET_LIBDIR=$(rustc --print target-libdir --target "$LIBRASHADER_RUST_TARGET")"
		# Ask Git for source inputs so generated *.cmake files inside build
		# directories cannot invalidate their own signature on every run.
		git -C "$ROOT_DIR" ls-files -co --exclude-standard -z -- \
			'CMakeLists.txt' ':(glob)**/CMakeLists.txt' ':(glob)**/*.cmake' \
			| sort -z \
			| while IFS= read -r -d '' input; do
				shasum -a 256 "$ROOT_DIR/$input"
			done
		# Source edits do not require project regeneration, but additions and
		# removals do because the generated target records the Swift file list.
		find "$SWIFT_DIR" -type f -name '*.swift' -print | sort
	} | shasum -a 256 | awk '{print $1}'
}

generate_project_if_needed() {
	local current_signature saved_signature
	current_signature="$(configuration_signature)"
	if [[ -f "$CONFIG_STAMP" ]]; then
		saved_signature="$(<"$CONFIG_STAMP")"
	else
		saved_signature=""
	fi

	if [[ -d "$PROJECT" && "$saved_signature" == "$current_signature" ]]; then
		echo "Reusing unchanged fast-build Xcode project:"
		echo "  $PROJECT"
		return
	fi

	echo "Generating development Xcode project (PCSX2 core LTO disabled)..."
	cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Xcode \
		-DCMAKE_SYSTEM_NAME=iOS \
		-DARMSX2_REAL_DEVICE=ON \
		-DLTO_PCSX2_CORE=OFF \
		-DARMSX2_BUNDLE_IDENTIFIER="$BUNDLE_ID" \
		-DARMSX2_DEVELOPMENT_TEAM="$TEAM_ID"
	printf '%s\n' "$current_signature" > "$CONFIG_STAMP"
}

refresh_generated_git_metadata() {
	local short_hash full_hash git_date pbxproj svnrev_file
	short_hash="$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || true)"
	full_hash="$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || true)"
	git_date="$(git -C "$ROOT_DIR" log -1 --format=%cd --date=local 2>/dev/null || true)"
	[[ -n "$short_hash" ]] || return 0

	pbxproj="$PROJECT/project.pbxproj"
	if [[ -f "$pbxproj" ]]; then
		SHORT_HASH="$short_hash" perl -0pi -e \
			's/ARMSX2_GIT_HASH=\\"[0-9A-Fa-f]+\\"/ARMSX2_GIT_HASH=\\"$ENV{SHORT_HASH}\\"/g' \
			"$pbxproj"
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

package_ipa() {
	if [[ ! -d "$APP_PATH" ]]; then
		echo "error: built app was not found at $APP_PATH" >&2
		exit 1
	fi

	case "$STAGING_DIR" in
		"$BUILD_DIR"/*) ;;
		*) echo "error: unsafe staging path: $STAGING_DIR" >&2; exit 1 ;;
	esac
	rm -rf "$STAGING_DIR"
	mkdir -p "$STAGING_DIR/Payload"
	ditto "$APP_PATH" "$STAGING_DIR/Payload/ARMSX2iOS.app"
	local staged_app="$STAGING_DIR/Payload/ARMSX2iOS.app"

	local controller_skins_dir="$ROOT_DIR/app/src/main/assets/app_icons/controller_skins"
	if [[ -d "$controller_skins_dir" ]]; then
		ditto "$controller_skins_dir" "$staged_app/controller_skins"
	fi

	if [[ -n "$SIGN_IDENTITY" || "$AD_HOC_SIGN" == "1" ]]; then
		if [[ ! -f "$ENTITLEMENTS_FILE" ]]; then
			echo "error: entitlements file was not found at $ENTITLEMENTS_FILE" >&2
			exit 1
		fi
		local identity="${SIGN_IDENTITY:--}"
		if [[ -d "$staged_app/Frameworks" ]]; then
			while IFS= read -r -d '' nested; do
				codesign --force --sign "$identity" --timestamp=none "$nested"
			done < <(find "$staged_app/Frameworks" -type d -name '*.framework' -print0)
			while IFS= read -r -d '' nested; do
				codesign --force --sign "$identity" --timestamp=none "$nested"
			done < <(find "$staged_app/Frameworks" -type f \( -name '*.dylib' -o -perm -111 \) -print0)
		fi
		codesign --force --sign "$identity" --entitlements "$ENTITLEMENTS_FILE" \
			--timestamp=none "$staged_app"
	fi

	local output_ipa="$BUILD_DIR/$IPA_NAME"
	if [[ -e "$output_ipa" ]]; then
		rm "$output_ipa"
	fi
	(cd "$STAGING_DIR" && zip -qry "$output_ipa" Payload)
	echo "Created development IPA:"
	echo "  $output_ipa"
}

require_tool cmake
require_tool xcodebuild
require_tool shasum
mkdir -p "$BUILD_DIR"
ensure_librashader_toolchain

if ! xcrun --sdk iphoneos --find metal >/dev/null 2>&1 \
	|| ! xcrun --sdk iphoneos --find metallib >/dev/null 2>&1; then
	echo "error: the iPhoneOS Metal compiler tools were not found." >&2
	exit 1
fi

generate_project_if_needed
if ! grep -q "ARMSX2_HAS_LIBRASHADER" "$PROJECT/project.pbxproj"; then
	echo "error: generated project omitted librashader; refusing to package a shader-disabled IPA." >&2
	exit 1
fi
refresh_generated_git_metadata

echo "Building development IPA with optimized native code and unoptimized Swift UI..."
set +e
xcodebuild \
	-project "$PROJECT" \
	-scheme ARMSX2iOS \
	-configuration Release \
	-sdk iphoneos \
	SWIFT_OPTIMIZATION_LEVEL=-Onone \
	SWIFT_COMPILATION_MODE=singlefile \
	SWIFT_ENABLE_BATCH_MODE=NO \
	COMPILER_INDEX_STORE_ENABLE=NO \
	DEBUG_INFORMATION_FORMAT=dwarf \
	COPY_PHASE_STRIP=NO \
	STRIP_INSTALLED_PRODUCT=NO \
	CODE_SIGNING_ALLOWED=NO \
	CODE_SIGNING_REQUIRED=NO \
	CODE_SIGN_IDENTITY="" \
	build 2>&1 | tee "$BUILD_LOG"
XCODEBUILD_STATUS=${PIPESTATUS[0]}
set -e

if [[ "$XCODEBUILD_STATUS" -ne 0 ]]; then
	exit "$XCODEBUILD_STATUS"
fi

package_ipa
