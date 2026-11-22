#!/usr/bin/env bash
# ARMSX2 sideload RELEASE APK (com.armsx2) — PGO=optimize, ROTATION-signed.
# This is the real distribution recipe. One core serves 4K and 16K page kernels.
#
# WHY ROTATION SIGNING IS MANDATORY: every published com.armsx2 sideload APK is
# signed with a v3 signing-lineage rotation (old debug key for API<=32, new
# release key for API33+). Ship an APK signed any other way and on-device
# updates over an existing install will be rejected. gradle cannot express the
# lineage, so we let gradle build/sign, then strip META-INF and re-sign with
# apksigner below.
#
# SECRETS: the release key + passwords are read from armsx2_keystore.properties
# (gitignored, next to this repo's android gradle root) or from RELEASE_* env
# vars. Nothing secret is hardcoded here. The debug key uses the public
# "android" password by design.
#
# Usage:
#   VC=<versionCode> VN=<versionName> tools/build-release-apk.sh [output.apk]
# Env overrides:
#   VC, VN                versionCode / versionName (required)
#   PROF                  merged .profdata for PGO   (default ~/Downloads/armsx2.profdata)
#   LINEAGE               signing lineage .bin       (default ~/Downloads/armsx2_signing_lineage.bin)
#   DEBUG_KS              old-signer keystore        (default ~/.android/debug.keystore)
#   RELEASE_KS/RELEASE_KS_PASS/RELEASE_KEY_ALIAS/RELEASE_KEY_PASS
#                         new-signer overrides (else read from armsx2_keystore.properties)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # platforms/android
GRADLE="$ROOT_DIR/gradlew"

VC="${VC:?set VC=<versionCode>}"
VN="${VN:?set VN=<versionName>}"
PROF="${PROF:-$HOME/Downloads/armsx2.profdata}"
LINEAGE="${LINEAGE:-$HOME/Downloads/armsx2_signing_lineage.bin}"
DEBUG_KS="${DEBUG_KS:-$HOME/.android/debug.keystore}"
OUTPUT_APK="${1:-$HOME/Downloads/ARMSX2-Refresh-${VN}-vc${VC}.apk}"

if [[ -z "${JAVA_HOME:-}" && -d "/Applications/Android Studio.app/Contents/jbr/Contents/Home" ]]; then
	export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
fi
if [[ -z "${ANDROID_HOME:-}" && -d "$HOME/Library/Android/sdk" ]]; then
	export ANDROID_HOME="$HOME/Library/Android/sdk"
fi
[[ -n "${ANDROID_HOME:-}" && -d "$ANDROID_HOME" ]] || { echo "error: ANDROID_HOME not set" >&2; exit 1; }

BT="$(find "$ANDROID_HOME/build-tools" -mindepth 1 -maxdepth 1 -type d | sort | tail -1)"
ZIPALIGN="$BT/zipalign"; APKSIGNER="$BT/apksigner"; AAPT="$BT/aapt2"

# --- resolve the release (new) signer from armsx2_keystore.properties or env ---
PROPS="$ROOT_DIR/armsx2_keystore.properties"
prop() { [[ -f "$PROPS" ]] && sed -n "s/^$1=//p" "$PROPS" | head -1; }
REL_KS="${RELEASE_KS:-$(prop storeFile)}"
REL_KS_PASS="${RELEASE_KS_PASS:-$(prop storePassword)}"
REL_KEY_ALIAS="${RELEASE_KEY_ALIAS:-$(prop keyAlias)}"
REL_KEY_PASS="${RELEASE_KEY_PASS:-$(prop keyPassword)}"
# storeFile in the properties file may be relative to the gradle root
[[ -n "$REL_KS" && ! -f "$REL_KS" && -f "$ROOT_DIR/$REL_KS" ]] && REL_KS="$ROOT_DIR/$REL_KS"

for kv in "REL_KS:$REL_KS" "REL_KS_PASS:$REL_KS_PASS" "REL_KEY_ALIAS:$REL_KEY_ALIAS" "REL_KEY_PASS:$REL_KEY_PASS"; do
	[[ -n "${kv#*:}" ]] || { echo "error: missing ${kv%%:*} (add armsx2_keystore.properties or set RELEASE_* env)" >&2; exit 1; }
done
# PROF is only read in optimize mode. Requiring it unconditionally made a regen
# impossible: PGO_MODE=generate produces the instrumented APK you play to CREATE a
# profile, so demanding one up front is a chicken-and-egg that fails instantly with
# "FATAL missing" naming a file the run does not use.
REQUIRED_FILES=("$DEBUG_KS" "$REL_KS" "$LINEAGE" "$ZIPALIGN" "$APKSIGNER")
[[ "${PGO_MODE:-optimize}" == "optimize" ]] && REQUIRED_FILES+=("$PROF")
for f in "${REQUIRED_FILES[@]}"; do
	[[ -e "$f" ]] || { echo "FATAL missing: $f" >&2; exit 1; }
done

WORK="$ROOT_DIR/app/build/rel-${VN}"; rm -rf "$WORK"; mkdir -p "$WORK"
BUILT="$ROOT_DIR/app/build/outputs/apk/github/release/app-github-release.apk"
CORE="lib/arm64-v8a/libemucore.so"

# Print the mode actually used, not a guess: this said "pgo=optimize" unconditionally
# even under PGO_MODE=generate, which is a lie in the one log you check when a profile
# collection turns up empty.
echo "=== building (pgo=${PGO_MODE:-optimize}) ==="
rm -f "$BUILT"
# GRADLE_EXTRA_ARGS carries the per-target properties (minSdk / ndkVersion / march)
# from build-release-targets.sh. Deliberately UNQUOTED: it is a list of -P flags, and
# quoting would hand gradle one argument containing spaces. Empty when unset, which is
# every direct invocation of this script.
# shellcheck disable=SC2086
"$GRADLE" -p "$ROOT_DIR" :app:assembleGithubRelease \
	${GRADLE_EXTRA_ARGS:-} \
	-Parmsx2.pgo="${PGO_MODE:-optimize}" \
	-Parmsx2.pgoProfile="$PROF" \
	-Parmsx2.versionCode="$VC" \
	-Parmsx2.versionName="$VN" \
	-Parmsx2.recTestHooks="${REC_TEST_HOOKS:-false}"
[[ -f "$BUILT" ]] || { echo "FATAL gradle produced no APK" >&2; exit 1; }
unzip -p "$BUILT" "$CORE" > "$WORK/libemucore.so" || { echo "FATAL $CORE missing" >&2; exit 1; }
sz=$(wc -c < "$WORK/libemucore.so")   # portable: release.yml runs this on Linux
echo "  libemucore.so = $((sz/1024/1024)) MB"
[[ "$sz" -gt 10000000 ]] || { echo "FATAL libemucore.so too small ($sz)" >&2; exit 1; }

# Drop gradle's signature; the lineage signing below replaces it.
UNSIGNED="$WORK/unsigned.apk"; ALIGNED="$WORK/aligned.apk"
cp -f "$BUILT" "$UNSIGNED"
zip -qd "$UNSIGNED" "META-INF/*" >/dev/null 2>&1 || true
"$ZIPALIGN" -f -P 16 4 "$UNSIGNED" "$ALIGNED"

# ROTATION SIGN: old debug key (API<=32) --next-signer--> new release key (API33+) + lineage.
rm -f "$OUTPUT_APK"
"$APKSIGNER" sign \
	--ks "$DEBUG_KS" --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android \
	--next-signer \
	--ks "$REL_KS" --ks-key-alias "$REL_KEY_ALIAS" --ks-pass "pass:$REL_KS_PASS" --key-pass "pass:$REL_KEY_PASS" \
	--lineage "$LINEAGE" \
	--in "$ALIGNED" --out "$OUTPUT_APK"

echo; echo "================= VERIFY ================="
echo "-- core present --"
unzip -l "$OUTPUT_APK" "$CORE" || { echo "FATAL core missing" >&2; exit 1; }
echo "-- Discord Social SDK --"
# ★ The SDK is resolved from $DISCORD_SDK_DIR at configure time and gated only on
# include/discordpp.h existing, so with the variable unset the build simply omits Discord --
# no error, no warning, nothing in the log. A whole release shipped that way (2.6.6.8) and it
# was only noticed after publication. Make it impossible to do silently again.
if [[ -n "${DISCORD_SDK_DIR:-}" ]]; then
	# Capture then match. Piping into `grep -q` under `set -o pipefail` is a false-failure
	# generator: grep exits on the first match, SIGPIPEs unzip, and pipefail reports the whole
	# pipeline as failed even though the library WAS found. That fired here on the first run.
	discord_listing=$(unzip -l "$OUTPUT_APK")
	case "$discord_listing" in
		*libdiscord_partner_sdk.so*) echo "  present" ;;
		*) echo "FATAL DISCORD_SDK_DIR is set but libdiscord_partner_sdk.so is not in the APK" >&2; exit 1 ;;
	esac
else
	echo "  WARNING: DISCORD_SDK_DIR unset -- this build has NO Discord integration." >&2
	echo "           Set it to the staged dir (include/ + arm64-v8a/ + .aar) before a release." >&2
fi
echo "-- 16k alignment --"
"$ZIPALIGN" -c -P 16 4 "$OUTPUT_APK" && echo "  zipalign OK"
echo "-- signer certs (expect BOTH old debug + new release) --"
"$APKSIGNER" verify --print-certs "$OUTPUT_APK" | grep -iE "SHA-256|Signer" | head
echo "-- version / package --"
"$AAPT" dump badging "$OUTPUT_APK" 2>/dev/null | grep -E "package: name|versionCode|versionName" | head -2
echo; echo "OUTPUT: $OUTPUT_APK"; shasum -a256 "$OUTPUT_APK"
echo "RELEASE-BUILD-DONE"
