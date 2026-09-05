#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
# SPDX-License-Identifier: GPL-3.0+

"""Does the MSVC project name every source CMake builds on every platform?

The two file lists are maintained by hand, in different files, in different syntaxes,
and only ONE of them is exercised on any given developer's machine. So they drift
silently and in the direction nobody is looking: this tree is developed on Linux and
macOS, so it is always the Windows list that rots, and it rots into a link failure no
amount of local testing can surface.

That is not hypothetical. A renderer since removed from the tree shipped five of its
seven translation units and six of its headers absent from the project file, and was
unbuildable on Windows for months -- caught only because someone tried a configuration
this box cannot compile. Eight more sources were still missing after that was fixed,
five of them nowhere near the GS.

⚠️ THIS IS A CONSISTENCY CHECK AND NOT A BUILD. It proves the project NAMES the file. It
cannot prove the file compiles under MSVC, and a pass here must never be reported as
"Windows builds".

## What it judges, and why the list is explicit

Only files that reach the PCSX2 target on every platform. Deciding that from the CMake
text alone needs a fixed-point over list variables that are declared at one nesting depth
and consumed at another -- `pcsx2LTOSources` is set unconditionally and consumed inside
both arms of an if(), which no simple depth rule gets right, and `pcsx2RDebugSources` is
declared and never consumed at all. A parser clever enough to be right about those is
also clever enough to be subtly wrong without anyone noticing.

So the unconditional lists are named here, and everything else is SKIPPED AND REPORTED.
An unchecked list is printed on every run, so under-coverage stays visible instead of
passing as silence -- if a new list appears and nobody adds it here, the output says so
rather than quietly shrinking the check. Re-derive the set by following each name from
its `set()` to the `target_sources(PCSX2 ...)` that consumes it.
"""

import re
import sys
from pathlib import Path

# Lists that reach the PCSX2 target regardless of platform or architecture. Verified by
# following each to its consumer: the depth-0 `target_sources(PCSX2 PRIVATE ...)` block,
# `pcsx2USB*` on the line below it, and `pcsx2LTOSources` -- which gathers the core,
# IPU, SPU2 and GS lists and is consumed in BOTH arms of if(LTO_PCSX2_CORE).
UNCONDITIONAL_LISTS = {
	"pcsx2Sources", "pcsx2Headers",
	"pcsx2IPUSources", "pcsx2IPUHeaders",
	"pcsx2SPU2Sources", "pcsx2SPU2Headers",
	"pcsx2GSSources", "pcsx2GSHeaders",
	"pcsx2CDVDSources", "pcsx2CDVDHeaders",
	"pcsx2DEV9Sources", "pcsx2DEV9Headers",
	"pcsx2PADSources", "pcsx2PADHeaders",
	"pcsx2RecordingSources",
	"pcsx2DebugToolsSources", "pcsx2DebugToolsHeaders",
	"pcsx2HostSources", "pcsx2HostHeaders",
	"pcsx2ImGuiSources", "pcsx2ImGuiHeaders",
	"pcsx2InputSources", "pcsx2InputHeaders",
	"pcsx2ps2Sources", "pcsx2ps2Headers",
	"pcsx2USBSources", "pcsx2USBHeaders",
}

SOURCE_SUFFIXES = (".cpp", ".c", ".h", ".hpp", ".inl", ".mm")
CODE_SUFFIXES = (".cpp", ".c", ".mm")


def cmake_lists(path: Path):
	"""{list name: [paths appended at if()-depth zero]}, and every list name seen.

	Entries appended inside any if() are dropped even from an unconditional list: a
	guarded append is a deliberate platform choice and cannot be judged from here.
	"""
	depth = 0
	current = None
	entries: dict[str, list[str]] = {}
	seen: set[str] = set()

	for raw in path.read_text().splitlines():
		line = raw.strip()
		low = line.lower()

		if re.match(r"^(if|foreach|while)\s*\(", low):
			depth += 1
			continue
		if re.match(r"^end(if|foreach|while)\s*\(", low) or low.startswith("else"):
			if low.startswith("end"):
				depth = max(0, depth - 1)
			current = None
			continue

		m = re.match(r"^set\s*\(\s*([A-Za-z0-9_]+)", line) or \
			re.match(r"^list\s*\(\s*APPEND\s+([A-Za-z0-9_]+)", line)
		if m:
			current = m.group(1)
			seen.add(current)
			if depth != 0:
				current = None
			continue

		if line.startswith(")"):
			current = None
			continue
		if current is None:
			continue

		cand = line.split("#", 1)[0].strip()
		if not cand or " " in cand or not cand.endswith(SOURCE_SUFFIXES):
			continue
		entries.setdefault(current, []).append(cand)

	return entries, seen


def cmake_target_sources(path: Path, target: str):
	"""[paths in the depth-0 `target_sources(<target> ...)` blocks of one CMakeLists].

	The small frontends name their files directly in target_sources() rather than
	building list variables, so cmake_lists() sees nothing in them. Same rule about
	if(): a guarded target_sources() is a platform choice and is not judged here.
	"""
	depth = 0
	inside = False
	found: list[str] = []

	for raw in path.read_text().splitlines():
		line = raw.strip()
		low = line.lower()

		if re.match(r"^(if|foreach|while)\s*\(", low):
			depth += 1
			continue
		if re.match(r"^end(if|foreach|while)\s*\(", low) or low.startswith("else"):
			if low.startswith("end"):
				depth = max(0, depth - 1)
			inside = False
			continue

		if re.match(r"^target_sources\s*\(\s*" + re.escape(target) + r"\b", line):
			inside = (depth == 0)
			continue
		if line.startswith(")"):
			inside = False
			continue
		if not inside:
			continue

		cand = line.split("#", 1)[0].strip()
		if not cand or " " in cand or not cand.endswith(SOURCE_SUFFIXES):
			continue
		found.append(cand)

	return found


def vcxproj_files(path: Path):
	text = path.read_text(errors="replace")
	# Case-folded: MSVC paths are case-insensitive and the project genuinely spells some
	# directories differently from CMake (Ipu vs IPU). A case-sensitive compare reports
	# those as missing, which is how a checker earns a reputation for crying wolf.
	return {m.group(1).replace("\\", "/").lower()
			for m in re.finditer(r'<Cl(?:Compile|Include)\s+Include="([^"]+)"', text)}


def report(vcx_name: str, base: Path, in_project: set, named: list, skipped: list):
	"""Compare one CMake file list against one project file. Returns files missing."""
	missing = []
	checked = 0
	for rel, origin in named:
		# Only judge files that exist. A stale CMake entry naming a deleted file is a
		# different defect, and CMake itself reports that one.
		if not (base / rel).is_file():
			continue
		checked += 1
		if rel.lower() not in in_project:
			missing.append((rel, origin))

	if missing:
		srcs = [m for m in missing if m[0].endswith(CODE_SUFFIXES)]
		print(f"{vcx_name} does not name {len(missing)} file(s) that CMake builds on "
			  f"every platform ({len(srcs)} of them sources):")
		for rel, origin in sorted(missing, key=lambda m: (not m[0].endswith(CODE_SUFFIXES), m[0])):
			kind = "SOURCE" if rel.endswith(CODE_SUFFIXES) else "header"
			print(f"    {kind:6}  {rel}    [{origin}]")
		print("\n  A missing SOURCE is a Windows link failure. A missing header only hides")
		print(f"  the file from the IDE. Add each to {vcx_name} AND {vcx_name}.filters.")
	else:
		print(f"ok    msvc file list: {vcx_name} names all {checked} unconditional "
			  f"CMake files")

	# Never let under-coverage read as a pass.
	if skipped:
		print(f"\n  not checked ({len(skipped)} platform/arch-conditional or unconsumed "
			  f"lists): {', '.join(skipped)}")
		print("  If one of these became unconditional, add it to UNCONDITIONAL_LISTS.")

	return missing


def main():
	root = Path(__file__).resolve().parents[2]
	rc = 0

	# --- pcsx2: the core, whose files come from list variables. ---
	cmake_path = root / "pcsx2" / "CMakeLists.txt"
	vcx_path = root / "pcsx2" / "pcsx2.vcxproj"
	if not cmake_path.is_file() or not vcx_path.is_file():
		print(f"check-msvc-filelist: missing {cmake_path} or {vcx_path}")
		return 2

	entries, seen = cmake_lists(cmake_path)
	named = [(rel, name) for name in sorted(UNCONDITIONAL_LISTS) for rel in entries.get(name, [])]
	skipped = sorted(n for n in seen
					 if n not in UNCONDITIONAL_LISTS and entries.get(n) and n.startswith("pcsx2"))
	if report("pcsx2.vcxproj", cmake_path.parent, vcxproj_files(vcx_path), named, skipped):
		rc = 1

	# --- pcsx2-gsrunner: the replay harness, which names its files inline. It is in
	# the solution, so an unlisted source is the same Windows link failure. ---
	for proj, target in (("pcsx2-gsrunner", "pcsx2-gsrunner"),):
		cmake_path = root / proj / "CMakeLists.txt"
		vcx_path = root / proj / f"{proj}.vcxproj"
		if not cmake_path.is_file() or not vcx_path.is_file():
			print(f"check-msvc-filelist: missing {cmake_path} or {vcx_path}")
			return 2
		named = [(rel, "target_sources") for rel in cmake_target_sources(cmake_path, target)]
		print()
		if report(f"{proj}.vcxproj", cmake_path.parent, vcxproj_files(vcx_path), named, []):
			rc = 1

	return rc


if __name__ == "__main__":
	sys.exit(main())
