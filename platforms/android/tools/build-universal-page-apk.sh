#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_APK=""
APPLICATION_ID="${ARMSX2_APPLICATION_ID:-}"
while [[ $# -gt 0 ]]; do
	case "$1" in
	--application-id)
		APPLICATION_ID="$2"
		shift 2
		;;
	*)
		if [[ -z "$OUTPUT_APK" ]]; then
			OUTPUT_APK="$1"
		else
			echo "error: unexpected argument '$1'" >&2
			exit 1
		fi
		shift
		;;
	esac
done
OUTPUT_APK="${OUTPUT_APK:-$HOME/Downloads/ARMSX2-Refresh-UniversalPage-Test.apk}"
WORK_DIR="$ROOT_DIR/app/build/universal-page-apk"
BASE_APK="$WORK_DIR/base-4k.apk"
APK_16K="$WORK_DIR/base-16k.apk"
UNSIGNED_APK="$WORK_DIR/universal-unsigned.apk"
ALIGNED_APK="$WORK_DIR/universal-aligned.apk"
LIB_STAGE="$WORK_DIR/lib-stage"

if [[ -z "${JAVA_HOME:-}" && -d "/Applications/Android Studio.app/Contents/jbr/Contents/Home" ]]; then
	export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
fi

if [[ -z "${ANDROID_HOME:-}" && -d "$HOME/Library/Android/sdk" ]]; then
	export ANDROID_HOME="$HOME/Library/Android/sdk"
fi

if [[ -z "${ANDROID_HOME:-}" || ! -d "$ANDROID_HOME" ]]; then
	echo "error: ANDROID_HOME is not set and no SDK was found at $HOME/Library/Android/sdk" >&2
	exit 1
fi

BUILD_TOOLS_DIR="$(find "$ANDROID_HOME/build-tools" -mindepth 1 -maxdepth 1 -type d | sort | tail -n 1)"
ZIPALIGN="$BUILD_TOOLS_DIR/zipalign"
APKSIGNER="$BUILD_TOOLS_DIR/apksigner"
DEBUG_KEYSTORE="${ANDROID_DEBUG_KEYSTORE:-$HOME/.android/debug.keystore}"

if [[ ! -x "$ZIPALIGN" || ! -x "$APKSIGNER" ]]; then
	echo "error: zipalign/apksigner not found in $BUILD_TOOLS_DIR" >&2
	exit 1
fi

if [[ ! -f "$DEBUG_KEYSTORE" ]]; then
	echo "error: debug keystore not found at $DEBUG_KEYSTORE" >&2
	exit 1
fi

build_core() {
	local page_size="$1"
	local lib_name="$2"
	local out_apk="$3"
	# Sideload build = the `github` product flavor (adds MANAGE_EXTERNAL_STORAGE
	# + STORAGE_ALL_FILES). Flavors qualify the task name and output path with
	# the flavor, so this is assembleGithubDebug → apk/github/debug/. The Play
	# AAB uses bundlePlayRelease instead (the play flavor stays SAF-only).
	local built_apk="$ROOT_DIR/app/build/outputs/apk/github/debug/app-github-debug.apk"

	# Optional test-package override: building as com.armsx2.fbfetch (or any other
	# application id) lets the test APK coexist with a stable installation instead
	# of replacing it. Default (unset) keeps the manifest's own application id.
	local id_args=()
	if [[ -n "$APPLICATION_ID" ]]; then
		id_args+=("-Parmsx2.applicationId=$APPLICATION_ID")
	fi

	"$ROOT_DIR/gradlew" -p "$ROOT_DIR" :app:cleanCxx
	"$ROOT_DIR/gradlew" -p "$ROOT_DIR" :app:assembleGithubDebug \
		-Parmsx2.hostPageSize="$page_size" \
		-Parmsx2.nativeLibName="$lib_name" \
		"${id_args[@]}"

	if [[ ! -f "$built_apk" ]]; then
		echo "error: Gradle did not produce $built_apk" >&2
		exit 1
	fi

	if ! unzip -l "$built_apk" "lib/arm64-v8a/lib${lib_name}.so" >/dev/null; then
		echo "error: $built_apk does not contain lib/arm64-v8a/lib${lib_name}.so" >&2
		exit 1
	fi

	cp -f "$built_apk" "$out_apk"
}

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR" "$LIB_STAGE/lib/arm64-v8a" "$(dirname "$OUTPUT_APK")"

echo "Building 4K emucore..."
build_core "0x1000" "emucore_4k" "$BASE_APK"

echo "Building 16K emucore..."
build_core "0x4000" "emucore_16k" "$APK_16K"

unzip -p "$BASE_APK" "lib/arm64-v8a/libemucore_4k.so" > "$LIB_STAGE/lib/arm64-v8a/libemucore_4k.so"
unzip -p "$APK_16K" "lib/arm64-v8a/libemucore_16k.so" > "$LIB_STAGE/lib/arm64-v8a/libemucore_16k.so"

cp -f "$BASE_APK" "$UNSIGNED_APK"
zip -qd "$UNSIGNED_APK" "META-INF/*" >/dev/null 2>&1 || true
zip -qd "$UNSIGNED_APK" "lib/arm64-v8a/libemucore_4k.so" "lib/arm64-v8a/libemucore_16k.so" >/dev/null 2>&1 || true

(cd "$LIB_STAGE" && zip -qr -0 "$UNSIGNED_APK" lib)

"$ZIPALIGN" -f -P 16 4 "$UNSIGNED_APK" "$ALIGNED_APK"
"$APKSIGNER" sign \
	--ks "$DEBUG_KEYSTORE" \
	--ks-key-alias androiddebugkey \
	--ks-pass pass:android \
	--key-pass pass:android \
	--out "$OUTPUT_APK" \
	"$ALIGNED_APK"

"$APKSIGNER" verify --verbose "$OUTPUT_APK" >/dev/null
"$ZIPALIGN" -c -P 16 4 "$OUTPUT_APK" >/dev/null

echo "Created universal page-size APK:"
echo "  $OUTPUT_APK"
shasum -a 256 "$OUTPUT_APK"
