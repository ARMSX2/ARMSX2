#!/usr/bin/env bash
#
# doctor.sh -- prove the measurement rig is the tree, before measuring with it.
#
# The recurring failure this exists to stop is not a build that breaks. It is a
# build that does not happen: a target named wrongly, a binary excluded from the
# default target, an edit to a file no link depends on. Nothing reports an error,
# the stale binary runs, and it produces a well-formed, reproducible, plausible
# number about code that is no longer in the tree. The tell is never the absence
# of an error -- it is a positive check that the thing under test executed.
#
# So every check here is positive, and every one of them either passes or names
# what is wrong. There is no "probably fine".
#
#   FRESHNESS   ninja's own dependency graph, via a dry run. Not timestamps:
#               a timestamp answers "was this built recently", and the question
#               is "was this built from THIS source", which only the graph knows.
#   SHADERS     the runtime shader tree must be a link to the source tree, not a
#               copy that a POST_BUILD step forgot to refresh.
#   ORPHANS     a test source declaring TEST() that no CMakeLists.txt names is
#               compiled into nothing, and its suite passes by not existing.
#
# Linux only. It shells out to ninja, GNU coreutils (sha256sum, date -r FILE,
# readlink -f) and checks a symlink layout that CMake only creates on Linux, so
# on macOS or BSD it would not fail cleanly -- `date -r` there takes an epoch,
# not a file, and would print a wrong mtime rather than an error.
#
# On success it writes rig-provenance.json into the build directory. That file is
# the certificate: copy it next to any artifact a run produces and the artifact
# can afterwards say which tree it came from. Its absence means the rig was never
# checked, which is a different thing from a check that failed.
#
# Exit status is 0 only if every check passed. It never pipes a command whose
# status it needs -- a pipeline reports its LAST element, which is how a build
# that never started once read as success.

set -uo pipefail

if [ "$(uname -s)" != Linux ]; then
	echo "doctor.sh: Linux only (needs ninja and GNU coreutils)" >&2
	exit 2
fi

SOURCE_DIR=""
BUILD_DIR=""
JSON_OUT=""
QUIET=0
WRITE_PROVENANCE=1

usage()
{
	cat <<'EOF'
usage: doctor.sh [--source-dir DIR] [--build-dir DIR] [--json PATH] [--quiet]
                 [--no-provenance]

  --source-dir DIR   source tree (default: the git root above this script)
  --build-dir DIR    build tree to check (default: <source>/build)
  --json PATH        where to write provenance (default: <build>/rig-provenance.json)
  --quiet            only report failures
  --no-provenance    check only; do not write the certificate
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--source-dir) SOURCE_DIR="$2"; shift 2 ;;
		--build-dir)  BUILD_DIR="$2";  shift 2 ;;
		--json)       JSON_OUT="$2";   shift 2 ;;
		--quiet)      QUIET=1;         shift ;;
		--no-provenance) WRITE_PROVENANCE=0; shift ;;
		-h|--help)    usage; exit 0 ;;
		*) echo "doctor.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
	esac
done

if [ -z "$SOURCE_DIR" ]; then
	SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fi
[ -n "$BUILD_DIR" ] || BUILD_DIR="$SOURCE_DIR/build"
[ -n "$JSON_OUT" ]  || JSON_OUT="$BUILD_DIR/rig-provenance.json"

FAILURES=0
NOTES=()

say()   { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
pass()  { say "  ok    $*"; }
fail()  { printf '  FAIL  %s\n' "$*" >&2; FAILURES=$((FAILURES + 1)); }
note()  { NOTES+=("$*"); }

say "rig doctor"
say "  source  $SOURCE_DIR"
say "  build   $BUILD_DIR"
say ""

# ---------------------------------------------------------------------------
# 0. The build directory exists and was configured from this source tree.
#
# Checking the source path back out of the cache catches the case where two
# worktrees share a build directory name and the wrong one is being measured --
# the binaries are perfectly fresh, just not for the tree you are reading.
# ---------------------------------------------------------------------------
CACHE="$BUILD_DIR/CMakeCache.txt"
if [ ! -f "$CACHE" ]; then
	fail "no CMakeCache.txt in $BUILD_DIR -- not a configured build directory"
	echo "" >&2
	echo "rig doctor: FAILED ($FAILURES)" >&2
	exit 1
fi

CACHED_SOURCE="$(sed -n 's/^Pcsx2_SOURCE_DIR:STATIC=//p' "$CACHE" | head -1)"
if [ -n "$CACHED_SOURCE" ] && [ "$CACHED_SOURCE" != "$SOURCE_DIR" ]; then
	# Compare resolved paths: /home/bmd/dev is a symlink to the real location,
	# so two spellings of one directory are not a finding.
	if [ "$(cd "$CACHED_SOURCE" 2>/dev/null && pwd -P)" != "$(cd "$SOURCE_DIR" && pwd -P)" ]; then
		fail "build dir was configured from $CACHED_SOURCE, not $SOURCE_DIR"
	fi
fi

BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$CACHE" | head -1)"
ENABLE_RIG="$(sed -n 's/^ENABLE_RIG:BOOL=//p' "$CACHE" | head -1)"
[ -n "$BUILD_TYPE" ] || BUILD_TYPE="unknown"
[ -n "$ENABLE_RIG" ] || ENABLE_RIG="OFF"

if [ "$ENABLE_RIG" != "ON" ]; then
	note "ENABLE_RIG is $ENABLE_RIG: the runners and gates are excluded from the default target, so a plain build leaves them behind. Reconfigure with -DENABLE_RIG=ON, or build the 'rig' target by name every time."
fi

# ---------------------------------------------------------------------------
# 1. FRESHNESS -- ask the build graph, not the clock.
# ---------------------------------------------------------------------------
TARGETS_FILE="$BUILD_DIR/rig-targets.txt"
if [ ! -f "$TARGETS_FILE" ]; then
	fail "no rig-targets.txt in $BUILD_DIR -- reconfigure (this build predates the rig target)"
else
	RIG_TARGETS=()
	RIG_BINARIES=()
	while IFS=$'\t' read -r tname tfile; do
		[ -n "$tname" ] || continue
		RIG_TARGETS+=("$tname")
		RIG_BINARIES+=("$tfile")
	done < "$TARGETS_FILE"

	if [ "${#RIG_TARGETS[@]}" -eq 0 ]; then
		fail "rig-targets.txt is empty -- nothing would be checked"
	elif [ ! -f "$BUILD_DIR/build.ninja" ]; then
		# Freshness is only decidable here through ninja. Say so rather than
		# skipping quietly: an unchecked rig and a checked one must not
		# produce the same output.
		fail "no build.ninja in $BUILD_DIR -- freshness is undecidable for this generator"
	else
		DRY="$(mktemp)"
		ninja -C "$BUILD_DIR" -n "${RIG_TARGETS[@]}" > "$DRY" 2>&1
		NINJA_STATUS=$?

		if [ "$NINJA_STATUS" -ne 0 ]; then
			# The archetypal case: an unknown target name. Ninja compiles
			# nothing at all and, piped, would have reported success.
			fail "ninja rejected the rig targets (exit $NINJA_STATUS):"
			sed 's/^/        /' "$DRY" >&2
		elif grep -q '^ninja: no work to do\.$' "$DRY"; then
			pass "freshness: all ${#RIG_TARGETS[@]} rig targets are current with the source"
			NINJA_CLEAN=1
		else
			PENDING="$(grep -cE '^\[[0-9]+/[0-9]+\]' "$DRY")"
			fail "freshness: $PENDING build steps outstanding -- these binaries are NOT this source"
			grep -E '^\[[0-9]+/[0-9]+\]' "$DRY" | head -12 | sed 's/^/        /' >&2
			[ "$PENDING" -gt 12 ] && echo "        ... and $((PENDING - 12)) more" >&2
			echo "        fix: cmake --build $BUILD_DIR --target rig" >&2
		fi
		rm -f "$DRY"
	fi
fi
NINJA_CLEAN="${NINJA_CLEAN:-0}"

# ---------------------------------------------------------------------------
# 2. SHADERS -- the runtime tree must be a link, not a copy.
# ---------------------------------------------------------------------------
LINKS_FILE="$BUILD_DIR/rig-links.txt"
if [ ! -f "$LINKS_FILE" ]; then
	fail "no rig-links.txt in $BUILD_DIR -- reconfigure with -DENABLE_RIG=ON (only a rig build links its resources)"
else
	LINK_OK=0
	LINK_N=0
	while IFS= read -r rel; do
		[ -n "$rel" ] || continue
		LINK_N=$((LINK_N + 1))
		dest="$BUILD_DIR/bin/$rel"
		# The source path is the same relative path under bin/, except for
		# entries staged flat from elsewhere -- those are not listed here.
		src="$SOURCE_DIR/bin/$rel"
		if [ ! -e "$dest" ]; then
			fail "resources: $dest does not exist -- reconfigure"
		elif [ ! -L "$dest" ]; then
			fail "resources: $dest is a COPY, not a link -- an edit to it will not reach the binary until the frontend relinks (reconfigure)"
		elif [ "$(readlink -f "$dest")" != "$(readlink -f "$src")" ]; then
			fail "resources: $dest resolves to $(readlink -f "$dest"), not $src"
		else
			LINK_OK=$((LINK_OK + 1))
		fi
	done < "$LINKS_FILE"
	if [ "$LINK_OK" -eq "$LINK_N" ] && [ "$LINK_N" -gt 0 ]; then
		pass "resources: $LINK_N edited trees are linked to the source, so an edit cannot be stale"
	fi
fi

# ---------------------------------------------------------------------------
# 3. ORPHANS -- a test file the build never sees.
#
# Distinct from staleness and the same shape of lie: the suite it belongs to
# passes, and it passes by not being there. Counting declared tests against the
# binary would catch it too, but this names the file directly and costs nothing.
# ---------------------------------------------------------------------------
ORPHANS=()
if [ -d "$SOURCE_DIR/tests/ctest" ]; then
	CMAKE_TEXT="$(mktemp)"
	find "$SOURCE_DIR/tests/ctest" -name CMakeLists.txt -exec cat {} + > "$CMAKE_TEXT" 2>/dev/null
	while IFS= read -r src; do
		grep -qE '^(TEST|TEST_F|TEST_P)\(' "$src" || continue
		base="$(basename "$src")"
		grep -qF "$base" "$CMAKE_TEXT" || ORPHANS+=("${src#"$SOURCE_DIR"/}")
	done < <(find "$SOURCE_DIR/tests/ctest" -name '*.cpp')
	rm -f "$CMAKE_TEXT"
fi

if [ "${#ORPHANS[@]}" -gt 0 ]; then
	fail "orphan test sources: declare TEST() but no CMakeLists.txt names them, so they compile into nothing"
	for o in "${ORPHANS[@]}"; do echo "        $o" >&2; done
else
	pass "orphans: every test source declaring TEST() is named by a CMakeLists.txt"
fi

# ---------------------------------------------------------------------------
# 4. MSVC FILE LIST -- the build nobody here can run.
#
# Same shape of lie as an orphan test, one platform over. CMake and the MSVC
# project name their files in separate hand-maintained lists, and only the CMake
# one is ever exercised on this box -- so the project rots into a Windows link
# failure that every local gate reports green. The Tile renderer was unbuildable
# there for three milestones, and eight more sources were missing beside it.
#
# ⚠️ A pass here is NOT "Windows builds". It is only "the project names the file".
# ---------------------------------------------------------------------------
if [ -f "$SOURCE_DIR/tools/rig/check-msvc-filelist.py" ]; then
	MSVC_OUT="$(python3 "$SOURCE_DIR/tools/rig/check-msvc-filelist.py" 2>&1)"
	MSVC_RC=$?
	# The skipped-lists tail prints on success too, so under-coverage never reads
	# as silence; keep it out of the one-line pass.
	if [ "$MSVC_RC" -eq 0 ]; then
		pass "$(printf '%s' "$MSVC_OUT" | sed -n 's/^ok    //p' | head -1)"
	else
		fail "msvc file list: pcsx2.vcxproj is missing files CMake builds everywhere"
		printf '%s\n' "$MSVC_OUT" >&2
	fi
fi

# ---------------------------------------------------------------------------
# 5. PROVENANCE -- the certificate.
# ---------------------------------------------------------------------------
GIT_SHA="$(git -C "$SOURCE_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
GIT_BRANCH="$(git -C "$SOURCE_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY=false
if [ -n "$(git -C "$SOURCE_DIR" status --porcelain 2>/dev/null)" ]; then
	GIT_DIRTY=true
	note "the source tree is dirty: the sha below names a commit the binaries were NOT built from exactly. Anything pinned from this rig is not re-derivable."
fi

if [ "$FAILURES" -eq 0 ] && [ "$WRITE_PROVENANCE" -eq 1 ]; then
	{
		printf '{\n'
		printf '  "checked_at": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf '  "source_dir": "%s",\n' "$SOURCE_DIR"
		printf '  "build_dir": "%s",\n' "$BUILD_DIR"
		printf '  "build_type": "%s",\n' "$BUILD_TYPE"
		printf '  "enable_rig": "%s",\n' "$ENABLE_RIG"
		printf '  "git_sha": "%s",\n' "$GIT_SHA"
		printf '  "git_branch": "%s",\n' "$GIT_BRANCH"
		printf '  "git_dirty": %s,\n' "$GIT_DIRTY"
		printf '  "ninja_clean": %s,\n' "$([ "$NINJA_CLEAN" -eq 1 ] && echo true || echo false)"
		printf '  "binaries": {\n'
		first=1
		i=0
		for t in "${RIG_TARGETS[@]:-}"; do
			bin="${RIG_BINARIES[$i]}"
			i=$((i + 1))
			[ -f "$bin" ] || continue
			sha="$(sha256sum "$bin" 2>/dev/null | cut -d' ' -f1)"
			mt="$(date -u -r "$bin" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)"
			[ "$first" -eq 1 ] || printf ',\n'
			first=0
			printf '    "%s": { "file": "%s", "sha256": "%s", "mtime": "%s" }' \
				"$t" "$(basename "$bin")" "$sha" "$mt"
		done
		printf '\n  }\n'
		printf '}\n'
	} > "$JSON_OUT"
	pass "provenance: $JSON_OUT"
fi

say ""
for n in "${NOTES[@]:-}"; do
	[ -n "$n" ] && printf 'note: %s\n' "$n"
done

if [ "$FAILURES" -ne 0 ]; then
	# Remove any earlier certificate. A failed check must not leave a passing
	# one behind for the next reader to trust.
	[ "$WRITE_PROVENANCE" -eq 1 ] && rm -f "$JSON_OUT"
	printf 'rig doctor: FAILED (%d)\n' "$FAILURES" >&2
	exit 1
fi

if [ "$GIT_DIRTY" = true ]; then
	say "rig doctor: ok at $GIT_SHA (dirty)"
else
	say "rig doctor: ok at $GIT_SHA"
fi
exit 0
