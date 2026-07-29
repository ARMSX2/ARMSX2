#!/usr/bin/env bash
#
# Source-based coverage for the ARM64 recompilers.
#
# Configures/builds the instrumented tree (build-coverage/), runs the gtest
# binaries, and reports line/region coverage scoped to the ARM64 sources --
# legacy x86 and shared PCSX2 code are filtered out at report time, so the
# percentage answers "how much of *our* JIT do the tests reach".
#
# Usage:
#   tools/coverage.sh                 # build, run, print the per-file summary
#   tools/coverage.sh --html          # ...and write a browsable HTML report
#   tools/coverage.sh --no-build      # reuse the existing build-coverage/ tree
#   tools/coverage.sh --uncovered     # list the fully-unreached functions
#   tools/coverage.sh --scope all     # widen from pcsx2/arm64/ to every ARM64 file
#
# Any trailing arguments are passed to llvm-cov as extra -sources filters.

set -euo pipefail

# pwd -P, not pwd: /home/bmd/dev and /home/bmd/ARMSX2 are symlinks into the real
# tree, and configuring through a symlinked source dir breaks Qt AUTOMOC. Always
# resolve to the physical path before handing anything to cmake.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SRC_DIR="$(dirname "${SCRIPT_DIR}")"
BUILD_DIR="${SRC_DIR}/build-coverage"
PROF_DIR="${BUILD_DIR}/profraw"
PROFDATA="${BUILD_DIR}/coverage.profdata"
HTML_DIR="${BUILD_DIR}/coverage-html"

LLVM_PROFDATA="${LLVM_PROFDATA:-llvm-profdata}"
LLVM_COV="${LLVM_COV:-llvm-cov}"

TESTS=(recompiler_tests core_test common_test gs_vertex_tests mvu_progcache_versioning_tests)

do_build=1
do_html=0
do_uncovered=0
scope=arm64
extra_sources=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--no-build)   do_build=0 ;;
		--html)       do_html=1 ;;
		--uncovered)  do_uncovered=1 ;;
		--scope)      scope="$2"; shift ;;
		-h|--help)    sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*)            extra_sources+=("$1") ;;
	esac
	shift
done

command -v "${LLVM_PROFDATA}" >/dev/null || { echo "error: ${LLVM_PROFDATA} not found" >&2; exit 1; }
command -v "${LLVM_COV}"      >/dev/null || { echo "error: ${LLVM_COV} not found" >&2; exit 1; }

# ---------------------------------------------------------------- build

if [[ ${do_build} -eq 1 ]]; then
	if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
		echo "==> configuring ${BUILD_DIR}"
		# cd first: --preset takes the source dir from the working directory and
		# ignores -S, so invoking this from the /home/bmd/ARMSX2 symlink would bake
		# the symlinked path into CMAKE_HOME_DIRECTORY (and into every coverage
		# mapping), which also trips the Qt AUTOMOC symlink bug.
		(cd "${SRC_DIR}" && cmake --preset clang-coverage)
	fi
	echo "==> building ${#TESTS[@]} test targets"
	cmake --build "${BUILD_DIR}" --target "${TESTS[@]}"
fi

# The prefix the compiler actually recorded in the coverage mappings. Deriving it
# from the cache rather than assuming ${SRC_DIR} is what makes --sources reliable:
# llvm-cov matches these by string, and a prefix that matches nothing is not an
# error -- it silently reports the whole tree, which reads as a plausible number.
[[ -f "${BUILD_DIR}/CMakeCache.txt" ]] || { echo "error: no ${BUILD_DIR}/CMakeCache.txt -- run without --no-build" >&2; exit 1; }
SRC_PREFIX="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt")"
[[ -n "${SRC_PREFIX}" ]] || { echo "error: could not read CMAKE_HOME_DIRECTORY from ${BUILD_DIR}/CMakeCache.txt" >&2; exit 1; }

# The report scope. pcsx2/arm64/ is the JIT proper; --scope all adds the other
# files that only ever compile on ARM64. Everything else in the tree is either
# shared with upstream PCSX2 or x86-only, and is not what we gate on.
sources=("${SRC_PREFIX}/pcsx2/arm64")
if [[ "${scope}" == "all" ]]; then
	sources+=(
		"${SRC_PREFIX}/pcsx2/GS/GSVector4_arm64.h"
		"${SRC_PREFIX}/pcsx2/GS/GSVector4i_arm64.h"
		"${SRC_PREFIX}/pcsx2/GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.cpp"
		"${SRC_PREFIX}/pcsx2/GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.cpp"
		"${SRC_PREFIX}/pcsx2/SPU2/spu2_neon.cpp"
	)
fi
sources+=("${extra_sources[@]+"${extra_sources[@]}"}")

for s in "${sources[@]}"; do
	[[ -e "${s}" ]] || { echo "error: coverage scope path does not exist: ${s}" >&2; exit 1; }
done

for t in "${TESTS[@]}"; do
	[[ -x "${BUILD_DIR}/bin/${t}" ]] || { echo "error: ${BUILD_DIR}/bin/${t} missing -- run without --no-build" >&2; exit 1; }
done

# ---------------------------------------------------------------- run

rm -rf "${PROF_DIR}"; mkdir -p "${PROF_DIR}"

failed=()
for t in "${TESTS[@]}"; do
	echo "==> running ${t}"
	# %p keeps forked gtest death-test children from clobbering the parent's file.
	if ! LLVM_PROFILE_FILE="${PROF_DIR}/${t}-%p.profraw" "${BUILD_DIR}/bin/${t}"; then
		failed+=("${t}")
	fi
done

if [[ ${#failed[@]} -gt 0 ]]; then
	echo
	echo "WARNING: these suites failed: ${failed[*]}"
	echo "         Coverage below still reflects the code they reached before failing."
fi

# ---------------------------------------------------------------- report

echo "==> merging profiles"
# shellcheck disable=SC2046
"${LLVM_PROFDATA}" merge -sparse -o "${PROFDATA}" $(find "${PROF_DIR}" -name '*.profraw')

objects=()
for t in "${TESTS[@]}"; do
	objects+=(-object "${BUILD_DIR}/bin/${t}")
done

src_args=()
for s in "${sources[@]}"; do
	src_args+=(-sources "${s}")
done

REPORT="${BUILD_DIR}/coverage-report.txt"
"${LLVM_COV}" report "${objects[@]}" -instr-profile="${PROFDATA}" \
	-show-region-summary=true -show-branch-summary=false \
	"${src_args[@]}" > "${REPORT}"

# Tripwire for the failure mode above: if --sources matched nothing, llvm-cov
# reports every file it has data for instead of erroring. The scoped report is a
# few dozen files; a few hundred means the filter fell off and the TOTAL is the
# whole tree, not the JIT.
rows="$(grep -cE '^[^ ].*[0-9]+\.[0-9]+%' "${REPORT}" || true)"
if [[ "${rows}" -gt 200 ]]; then
	echo "error: report lists ${rows} files -- the --sources filter did not apply." >&2
	echo "       Coverage data is keyed to '${SRC_PREFIX}'; check that path is right." >&2
	exit 1
fi

echo
cat "${REPORT}"

if [[ ${do_uncovered} -eq 1 ]]; then
	echo
	echo "==> functions with zero coverage"
	"${LLVM_COV}" report "${objects[@]}" -instr-profile="${PROFDATA}" \
		-show-functions "${src_args[@]}" 2>/dev/null |
		awk '$0 !~ /^(Filename|---|TOTAL)/ && NF >= 4 && $4 == "0" { print }' | head -80
fi

if [[ ${do_html} -eq 1 ]]; then
	rm -rf "${HTML_DIR}"
	"${LLVM_COV}" show "${objects[@]}" -instr-profile="${PROFDATA}" \
		-format=html -output-dir="${HTML_DIR}" \
		-show-line-counts-or-regions -show-expansions \
		"${src_args[@]}"
	echo
	echo "HTML report: ${HTML_DIR}/index.html"
fi
