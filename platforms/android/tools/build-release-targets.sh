#!/usr/bin/env bash
# ARMSX2 multi-target release driver.
#
# Builds the sideload APK for one or both CPU targets by delegating to
# build-release-apk.sh, which still owns the whole recipe (dual page-size cores, PGO,
# rotation signing). This script's only job is the target matrix and the file NAMING,
# because the in-app updater picks its download by filename:
#
#   ARMSX2-<VN>.apk           legacy    — every device that runs ARMSX2 today
#   ARMSX2-<VN>-v82.apk       standard  — Android 13+, needs FEAT_FP16 + FEAT_DotProd
#   ARMSX2-<VN>-v82-sdk35.apk modern    — Android 15+, same CPU features, newest NDK
#
# Those filename markers are a CONTRACT with UpdaterEntry.kt. Rename an artifact and the
# devices it was built for silently fall back a tier — quietly, with no error anywhere.
#
# THE LEGACY BUILD MUST SHIP IN EVERY RELEASE. The updater hands it to any device it
# cannot positively confirm, so a release without it stops updates dead for everything
# older. `all` is the default for exactly that reason.
#
# Usage:
#   VC=<versionCode> VN=<versionName> tools/build-release-targets.sh [all|legacy|standard|modern]
# Env: everything build-release-apk.sh takes (PROF, PGO_MODE, LINEAGE, RELEASE_*...).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VC="${VC:?set VC=<versionCode>}"
VN="${VN:?set VN=<versionName>}"
WHICH="${1:-all}"
OUTDIR="${OUTDIR:-$HOME/Downloads}"

# --- the three targets ----------------------------------------------------------------
# legacy:   deliberately frozen. Same minSdk and NDK that shipped 2.6.6.6, so it stays a
#           known quantity and the ONLY moving part between releases is our own source. The
#           march is stated explicitly rather than left to BuildParameters.cmake's default,
#           so this file declares its own contract instead of inheriting one that could
#           change under it silently. Same codegen either way.
# standard: the CPU upgrade. FEAT_FP16 and FEAT_DotProd are OPTIONAL at ARMv8.2, so they
#           are named explicitly — "-march=armv8.2-a" alone would enable neither, and the
#           updater's probe checks for precisely these two HWCAP flags (asimdhp/asimddp).
#           Keep the two in step: widen the march here and the probe must widen with it,
#           or the updater will hand this build to a CPU that cannot run it (SIGILL, and
#           the user cannot reach the updater to escape it).
# modern:   same codegen as standard; the difference is platform, not instructions —
#           minSdk 35 and NDK 29. Worth measuring before it earns a permanent slot: if it
#           does not separate from standard on a device, three artifacts is three times
#           the release surface for nothing.
LEGACY_MINSDK=26;   LEGACY_NDK="28.2.13676358";   LEGACY_MARCH="armv8.1-a"
STANDARD_MINSDK=33; STANDARD_NDK="28.2.13676358"; STANDARD_MARCH="armv8.2-a+fp16+dotprod"
MODERN_MINSDK=35;   MODERN_NDK="29.0.14206865";   MODERN_MARCH="armv8.2-a+fp16+dotprod"

build_one() { # label minsdk ndk march outfile
	local label="$1" minsdk="$2" ndk="$3" march="$4" out="$5"
	echo
	echo "############ target: $label ############"
	echo "  minSdk=$minsdk  ndk=$ndk  march=${march:-<cmake default: armv8.1-a>}"
	echo "  -> $out"
	GRADLE_EXTRA_ARGS="-Parmsx2.minSdk=$minsdk -Parmsx2.ndkVersion=$ndk${march:+ -Parmsx2.march=$march}" \
		VC="$VC" VN="$VN" "$HERE/build-release-apk.sh" "$out"
}

case "$WHICH" in
	all|legacy|standard|modern) : ;;
	*) echo "usage: $0 [all|legacy|standard|modern]" >&2; exit 1 ;;
esac

if [[ "$WHICH" == "legacy" || "$WHICH" == "all" ]]; then
	build_one legacy "$LEGACY_MINSDK" "$LEGACY_NDK" "$LEGACY_MARCH" \
		"$OUTDIR/ARMSX2-${VN}.apk"
fi
if [[ "$WHICH" == "standard" || "$WHICH" == "all" ]]; then
	build_one standard "$STANDARD_MINSDK" "$STANDARD_NDK" "$STANDARD_MARCH" \
		"$OUTDIR/ARMSX2-${VN}-v82.apk"
fi
if [[ "$WHICH" == "modern" || "$WHICH" == "all" ]]; then
	build_one modern "$MODERN_MINSDK" "$MODERN_NDK" "$MODERN_MARCH" \
		"$OUTDIR/ARMSX2-${VN}-v82-sdk35.apk"
fi

# --- release-shape check --------------------------------------------------------------
# Fails loudly rather than letting a v82-only release reach GitHub, where it would look
# fine and quietly strand every pre-v8.2 device on its installed version forever.
if [[ "$WHICH" == "all" ]]; then
	echo
	echo "================= RELEASE SHAPE ================="
	legacy="$OUTDIR/ARMSX2-${VN}.apk"
	standard="$OUTDIR/ARMSX2-${VN}-v82.apk"
	modern="$OUTDIR/ARMSX2-${VN}-v82-sdk35.apk"
	for f in "$legacy" "$standard" "$modern"; do
		[[ -f "$f" ]] || { echo "FATAL artifact missing: $f" >&2; exit 1; }
	done
	# The updater classifies by marker, so the legacy build's own name must carry neither
	# (a VN someone set to "2.7.0-v82-test" would make the legacy APK look like a v8.2 one
	# and strand every pre-v8.2 device).
	case "$(basename "$legacy")" in *-v82*|*-sdk35*)
		echo "FATAL legacy filename contains a tier marker" >&2; exit 1 ;; esac
	# -sdk35 must be strictly more specific than -v82: the updater tests for it FIRST, so a
	# standard build that also matched would be picked for Android 15 devices.
	case "$(basename "$standard")" in *-sdk35*)
		echo "FATAL standard filename contains -sdk35" >&2; exit 1 ;; esac
	for f in "$legacy" "$standard" "$modern"; do
		printf "  %-36s %s\n" "$(basename "$f")" "$(ls -lh "$f" | awk '{print $5}')"
	done
	echo "  all three present, markers correct — safe to publish"
fi
