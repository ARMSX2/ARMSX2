#!/usr/bin/env bash
# ============================================================================
# ci-nightly.sh — GitHub Actions PGO nightly APK.
# ----------------------------------------------------------------------------
# The CI-portable (Linux, no macOS assumptions) version of
# tools/build-release-apk.sh: one gradle build with PGO=optimize so the nightly
# matches release performance, then rotation signing. The one core reads the
# host page size when it loads, so the APK runs on 4K and 16K page kernels.
#
# Runs from platforms/android (the gradle root); the nightly workflow sets
# working-directory accordingly.
#
# Env:
#   VC, VN                versionCode / versionName            (required)
#   FLAVOR                Github (sideload) | Play (default Github)
#   APK_ID                applicationId (default com.armsx2.nightly). Nightlies use their own id
#                         so they install ALONGSIDE the stable com.armsx2 app instead of replacing
#                         it; the two therefore never share a signing requirement or an updater.
#   APP_LABEL             android:label override (default @string/app_name_nightly)
#   PROF                  merged .profdata  (default pgo/armsx2.profdata; if the
#                         file is absent the build falls back to PGO=none rather
#                         than failing, so a missing profile never breaks CI)
#   OUT                   output apk path
#   NIGHTLY_KEYSTORE / NIGHTLY_KS_PASS / NIGHTLY_KEY_ALIAS / NIGHTLY_KEY_PASS
#                         stable signing key (recommended: add as a repo secret
#                         so nightly-over-nightly updates install; otherwise a
#                         throwaway debug key is generated and users must
#                         reinstall between nightlies).
# ============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # platforms/android
GRADLE="$ROOT_DIR/gradlew"

VC="${VC:?set VC=<versionCode>}"
VN="${VN:?set VN=<versionName>}"
FLAVOR="${FLAVOR:-Github}"                                    # Github | Play
flavor_lc="$(printf '%s' "$FLAVOR" | tr '[:upper:]' '[:lower:]')"
APK_ID="${APK_ID:-com.armsx2.nightly}"                        # separate package -> side-by-side install
APP_LABEL="${APP_LABEL:-@string/app_name_nightly}"
PROF="${PROF:-$ROOT_DIR/pgo/armsx2.profdata}"
OUT="${OUT:-$ROOT_DIR/app/build/outputs/apk/nightly/ARMSX2-nightly-${VN}-vc${VC}.apk}"

# --- resolve Android SDK build-tools (Linux CI exports ANDROID_HOME/SDK_ROOT) ---
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[[ -n "$SDK" && -d "$SDK" ]] || { echo "FATAL: ANDROID_HOME/ANDROID_SDK_ROOT not set" >&2; exit 1; }
BT="$(find "$SDK/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)"
ZIPALIGN="$BT/zipalign"; APKSIGNER="$BT/apksigner"; AAPT="$BT/aapt2"
# aapt2 is required (not optional): the VERIFY step uses it to assert the APK really carries
# $APK_ID, and a missing tool must fail the build rather than silently skip that check.
for t in "$ZIPALIGN" "$APKSIGNER" "$AAPT"; do
	[[ -x "$t" ]] || { echo "FATAL: missing build-tool $t" >&2; exit 1; }
done

# --- PGO: optimize when the profile exists, else fall back so CI never fails ---
if [[ -f "$PROF" ]]; then
	PGO_ARGS=(-Parmsx2.pgo=optimize -Parmsx2.pgoProfile="$PROF")
	echo "PGO: optimize using $PROF ($(( $(wc -c < "$PROF") / 1024 / 1024 )) MB)"
else
	PGO_ARGS=(-Parmsx2.pgo=none)
	echo "WARNING: PGO profile not found at $PROF — building PGO=none (slower core)." >&2
fi

WORK="$ROOT_DIR/app/build/nightly"; rm -rf "$WORK"
mkdir -p "$WORK" "$(dirname "$OUT")"
BUILT="$ROOT_DIR/app/build/outputs/apk/${flavor_lc}/release/app-${flavor_lc}-release.apk"
CORE="lib/arm64-v8a/libemucore.so"

fsize() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1"; }   # Linux || macOS

echo "=== flavor=$FLAVOR, id=$APK_ID ==="
rm -f "$BUILT"
"$GRADLE" -p "$ROOT_DIR" ":app:assemble${FLAVOR}Release" \
	"${PGO_ARGS[@]}" \
	-Parmsx2.versionCode="$VC" \
	-Parmsx2.versionName="$VN" \
	-Parmsx2.applicationId="$APK_ID" \
	-Parmsx2.channel=nightly \
	-Parmsx2.appLabel="$APP_LABEL" \
	--stacktrace
[[ -f "$BUILT" ]] || { echo "FATAL: gradle produced no APK ($BUILT)" >&2; exit 1; }
unzip -p "$BUILT" "$CORE" > "$WORK/libemucore.so" \
	|| { echo "FATAL: $CORE missing from $BUILT" >&2; exit 1; }
sz="$(fsize "$WORK/libemucore.so")"
echo "  libemucore.so = $(( sz / 1024 / 1024 )) MB"
[[ "$sz" -gt 10000000 ]] || { echo "FATAL: libemucore.so too small ($sz bytes)" >&2; exit 1; }

# --- drop gradle's signature; the lineage signing below replaces it ---
UNS="$WORK/unsigned.apk"; ALN="$WORK/aligned.apk"
cp -f "$BUILT" "$UNS"
zip -qd "$UNS" "META-INF/*" >/dev/null 2>&1 || true
"$ZIPALIGN" -f -P 16 4 "$UNS" "$ALN"

# --- sign ---------------------------------------------------------------------
# Preferred: the SAME v3 rotation lineage as the hand-built releases (old debug
# key for API<=32 --next-signer--> release key for API33+). Nightlies now ship a
# distinct package (com.armsx2.nightly) and install beside the stable app, so this
# no longer bridges to com.armsx2 — what it still guarantees is that every nightly
# carries the SAME certificate, so one nightly updates over the previous nightly in
# place. The three keystores + lineage arrive as repo secrets, decoded to files by
# the workflow and passed in as ROTATION_* env vars. If they're absent we fall back
# to a throwaway key and LOUDLY warn (that APK won't update over installed builds).
rm -f "$OUT"
if [[ -n "${ROTATION_DEBUG_KS:-}"   && -f "${ROTATION_DEBUG_KS:-/nope}"   \
   && -n "${ROTATION_RELEASE_KS:-}" && -f "${ROTATION_RELEASE_KS:-/nope}" \
   && -n "${ROTATION_LINEAGE:-}"    && -f "${ROTATION_LINEAGE:-/nope}"    \
   && -n "${ROTATION_RELEASE_KEY_ALIAS:-}" && -n "${ROTATION_RELEASE_KS_PASS:-}" ]]; then
	echo "Signing with the release rotation lineage (debug<=API32 -> release>=API33)."
	"$APKSIGNER" sign \
		--ks "$ROTATION_DEBUG_KS" --ks-key-alias "${ROTATION_DEBUG_ALIAS:-androiddebugkey}" \
		--ks-pass "pass:${ROTATION_DEBUG_PASS:-android}" --key-pass "pass:${ROTATION_DEBUG_PASS:-android}" \
		--next-signer \
		--ks "$ROTATION_RELEASE_KS" --ks-key-alias "$ROTATION_RELEASE_KEY_ALIAS" \
		--ks-pass "pass:$ROTATION_RELEASE_KS_PASS" --key-pass "pass:${ROTATION_RELEASE_KEY_PASS:-$ROTATION_RELEASE_KS_PASS}" \
		--lineage "$ROTATION_LINEAGE" \
		--in "$ALN" --out "$OUT"
else
	# FAIL CLOSED. A throwaway-signed nightly cannot update over a previously
	# rotation-signed nightly (signature mismatch -> "App not installed"), and once
	# published it strands every user who installs it — a certificate change can
	# never update in place. So refuse to build rather than ship a non-updatable APK
	# to a public release. (This is exactly how the first nightly-20260713 stranded
	# users before the secrets were wired up.) Provide the ROTATION_* secrets; do not
	# remove this guard.
	echo "FATAL: ROTATION_* signing secrets not set — refusing to build a nightly that" >&2
	echo "       would NOT install over existing nightlies. Set ROTATION_DEBUG_KS," >&2
	echo "       ROTATION_RELEASE_KS, ROTATION_LINEAGE, ROTATION_RELEASE_KEY_ALIAS and" >&2
	echo "       ROTATION_RELEASE_KS_PASS (see nightly.yml secrets)." >&2
	exit 1
fi

echo; echo "================= VERIFY ================="
echo "-- core present --"
unzip -l "$OUT" "$CORE" || { echo "FATAL: core missing" >&2; exit 1; }
echo "-- 16k alignment --"; "$ZIPALIGN" -c -P 16 4 "$OUT" && echo "  align OK"
echo "-- signature --"; "$APKSIGNER" verify "$OUT" && echo "  sig OK"
# The whole point of APK_ID is a side-by-side install; if the -P property ever fails to reach AGP
# the APK silently comes out as com.armsx2 again and would update over the stable app instead.
# Fail closed rather than publish that. (aapt2 is required at the top of the script, so this
# cannot be skipped by a missing tool.)
badging="$("$AAPT" dump badging "$OUT" 2>/dev/null)"
echo "$badging" | grep -E "package: name|versionCode|versionName" | head -2
echo "$badging" | grep -q "package: name='${APK_ID}'" \
	|| { echo "FATAL: APK package is not ${APK_ID} — applicationId property did not take" >&2; exit 1; }
echo; echo "OUTPUT: $OUT"
echo "NIGHTLY-DONE"
