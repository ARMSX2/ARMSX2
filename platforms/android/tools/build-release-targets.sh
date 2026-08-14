#!/usr/bin/env bash
# ARMSX2 multi-target release driver.
#
# Builds the sideload APK for one or all four CPU/OS targets by delegating to
# build-release-apk.sh, which still owns the whole recipe (dual page-size cores, PGO,
# rotation signing). This script's only job is the target matrix and the file NAMING,
# because the in-app updater picks its download by filename:
#
#   ARMSX2-<VN>-legacy-armv8.1-sdk26.apk   Android 8+,  armv8.1-a — every device today
#   ARMSX2-<VN>-a11-armv8.2-sdk30.apk      Android 11+, armv8.2 + FP16 + DotProd
#   ARMSX2-<VN>-a13-armv8.2-sdk33.apk      Android 13+, same codegen
#   ARMSX2-<VN>-a15-armv8.2-sdk35.apk      Android 15+, same codegen, newest NDK
#
# The `-sdkNN` suffix is the CONTRACT with UpdaterEntry.kt — it classifies by that and
# nothing else. The markers are mutually exclusive on purpose: the previous scheme used
# `-v82` and `-v82-sdk35`, where one marker contained the other and only a carefully
# ordered `when` kept Android 15 devices off the wrong build. Rename an artifact and the
# devices it was built for silently fall back a tier, quietly, with no error anywhere.
#
# THE LEGACY BUILD MUST SHIP IN EVERY RELEASE. The updater hands it to any device it
# cannot positively confirm, so a release without it stops updates dead for everything
# older. `all` is the default for exactly that reason.
#
# UPLOAD THE LEGACY APK FIRST. Every ARMSX2 up to 2.6.6.6 shipped an updater that takes
# the FIRST .apk asset in the release, whatever it is named — it predates tiering and
# cannot tell these apart. GitHub lists assets in upload order, so whichever goes up
# first is what every already-installed copy of the app will download. Upload legacy
# first and those users get the build they can run; upload a v8.2 one first and every
# pre-v8.2 device is handed an APK that SIGILLs on its first hot path.
#
# Usage:
#   VC=<versionCode> VN=<versionName> tools/build-release-targets.sh [all|legacy|a11|a13|a15]
# Env: everything build-release-apk.sh takes (PROF, PGO_MODE, LINEAGE, RELEASE_*...).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VC="${VC:?set VC=<versionCode>}"
VN="${VN:?set VN=<versionName>}"
WHICH="${1:-all}"
OUTDIR="${OUTDIR:-$HOME/Downloads}"

# --- the four targets -----------------------------------------------------------------
# legacy: deliberately frozen. Same minSdk and NDK that shipped 2.6.6.6, so it stays a
#         known quantity and the ONLY moving part between releases is our own source. The
#         march is stated explicitly rather than left to BuildParameters.cmake's default,
#         so this file declares its own contract instead of inheriting one that could
#         change under it silently. Same codegen either way.
# a11:    the lowest rung that gets the CPU upgrade. FEAT_FP16 and FEAT_DotProd are
#         OPTIONAL at ARMv8.2, so they are named explicitly — "-march=armv8.2-a" alone
#         would enable neither, and the updater's probe checks for precisely these two
#         HWCAP flags (asimdhp/asimddp). Keep the two in step: widen the march here and
#         the probe must widen with it, or the updater will hand this build to a CPU that
#         cannot run it (SIGILL, and the user cannot reach the updater to escape it).
# a13:    same codegen as a11; the difference is platform floor, not instructions.
# a15:    same codegen again, but built with NDK 29. That NDK — not the march — is where
#         the measured ~9% came from, so this tier is the one carrying a real delta.
LEGACY_MINSDK=26; LEGACY_NDK="28.2.13676358"; LEGACY_MARCH="armv8.1-a"
A11_MINSDK=30;    A11_NDK="28.2.13676358";    A11_MARCH="armv8.2-a+fp16+dotprod"
A13_MINSDK=33;    A13_NDK="28.2.13676358";    A13_MARCH="armv8.2-a+fp16+dotprod"
A15_MINSDK=35;    A15_NDK="29.0.14206865";    A15_MARCH="armv8.2-a+fp16+dotprod"

LEGACY_OUT="ARMSX2-${VN}-legacy-armv8.1-sdk26.apk"
A11_OUT="ARMSX2-${VN}-a11-armv8.2-sdk30.apk"
A13_OUT="ARMSX2-${VN}-a13-armv8.2-sdk33.apk"
A15_OUT="ARMSX2-${VN}-a15-armv8.2-sdk35.apk"

build_one() { # label minsdk ndk march outfile
	local label="$1" minsdk="$2" ndk="$3" march="$4" out="$5"
	echo
	echo "############ target: $label ############"
	echo "  minSdk=$minsdk  ndk=$ndk  march=$march"
	echo "  -> $out"
	GRADLE_EXTRA_ARGS="-Parmsx2.minSdk=$minsdk -Parmsx2.ndkVersion=$ndk -Parmsx2.march=$march" \
		VC="$VC" VN="$VN" "$HERE/build-release-apk.sh" "$out"
}

case "$WHICH" in
	all|legacy|a11|a13|a15) : ;;
	*) echo "usage: $0 [all|legacy|a11|a13|a15]" >&2; exit 1 ;;
esac

if [[ "$WHICH" == "legacy" || "$WHICH" == "all" ]]; then
	build_one legacy "$LEGACY_MINSDK" "$LEGACY_NDK" "$LEGACY_MARCH" "$OUTDIR/$LEGACY_OUT"
fi
if [[ "$WHICH" == "a11" || "$WHICH" == "all" ]]; then
	build_one a11 "$A11_MINSDK" "$A11_NDK" "$A11_MARCH" "$OUTDIR/$A11_OUT"
fi
if [[ "$WHICH" == "a13" || "$WHICH" == "all" ]]; then
	build_one a13 "$A13_MINSDK" "$A13_NDK" "$A13_MARCH" "$OUTDIR/$A13_OUT"
fi
if [[ "$WHICH" == "a15" || "$WHICH" == "all" ]]; then
	build_one a15 "$A15_MINSDK" "$A15_NDK" "$A15_MARCH" "$OUTDIR/$A15_OUT"
fi

# --- release-shape check --------------------------------------------------------------
# Fails loudly rather than letting an ARMv8.2-only release reach GitHub, where it would
# look fine and quietly strand every pre-v8.2 device on its installed version forever.
if [[ "$WHICH" == "all" ]]; then
	echo
	echo "================= RELEASE SHAPE ================="
	for f in "$LEGACY_OUT" "$A11_OUT" "$A13_OUT" "$A15_OUT"; do
		[[ -f "$OUTDIR/$f" ]] || { echo "FATAL artifact missing: $OUTDIR/$f" >&2; exit 1; }
	done
	# The updater classifies purely by the -sdkNN suffix, so each artifact must carry
	# exactly one and they must be distinct. A VN someone set to "2.7.0-sdk35-test" would
	# otherwise make the legacy APK look like the Android 15 build and hand it to devices
	# whose CPU cannot execute it.
	for pair in "$LEGACY_OUT:sdk26" "$A11_OUT:sdk30" "$A13_OUT:sdk33" "$A15_OUT:sdk35"; do
		name="${pair%:*}"; want="${pair##*:}"
		got="$(grep -o 'sdk[0-9][0-9]' <<<"$name" | sort -u | tr '\n' ' ')"
		[[ "$got" == "$want " ]] || {
			echo "FATAL $name carries sdk markers '$got', expected exactly '$want'" >&2; exit 1; }
	done
	for f in "$LEGACY_OUT" "$A11_OUT" "$A13_OUT" "$A15_OUT"; do
		printf "  %-44s %s\n" "$f" "$(ls -lh "$OUTDIR/$f" | awk '{print $5}')"
	done
	echo "  all four present, markers correct — safe to publish"
	echo
	echo "  UPLOAD ORDER: $LEGACY_OUT FIRST."
	echo "  Updaters shipped in 2.6.6.6 and earlier take the first .apk asset in the"
	echo "  release regardless of name; anything else first hands them an unrunnable build."
fi
