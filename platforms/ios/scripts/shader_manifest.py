#!/usr/bin/env python3
"""Resolve slang-shader preset closures off-device and describe them for a catalogue.

    python3 shader_manifest.py classify --checkout <pinned slang-shaders tree>
    python3 shader_manifest.py emit     --checkout <tree> --rules <signed rules file>

A preset's dependencies live inside file bodies, not in any index, so nothing upstream
can say what a preset needs before it is read. Reading them on a phone is a sequential
network walk of unbounded depth; reading them once here, against a local clone, answers
the transport question and the licence question with one artifact.

`classify` needs no human input. It produces the ballot, the per-file evidence table, a
deterministic sample per class and a reconciliation table. `emit` produces the manifest
and the zips and refuses to start without a signed rules file recording this pin, so the
catalogue cannot physically exist before a person has signed the rules it was built from.

The script never states a verdict. It assigns an evidence CLASS -- what a file's own
header says, with line numbers -- and leaves the verdict empty. A verdict is a statement
about redistributability and only the signed rules turn one into the other.
"""

import argparse
import csv
import datetime
import gzip
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_OUT = ROOT / "platforms/ios/build-ios-shader-manifest"
DEFAULT_PATCHES = ROOT / "platforms/ios/patches"
BRIDGE = ROOT / "platforms/ios/app/src/main/cpp/ARMSX2Bridge.mm"

# The patch context lines were cut against files byte-identical to this commit, and the
# row-for-row cross-check against the bundled-preset audit holds only here, because its
# closure byte counts are this tree's. Resolving against a newer master would break
# both silently.
PIN = "80372284ea8c00ae5e25e5a6e4f9f49415f85896"

# Borrowed from ShaderPresetLibrary.swift:47 rather than rediscovered: a reference chain
# deeper than this is reported unresolved, and a `visited` set is what makes a cycle
# terminate instead of hanging.
MAX_DEPTH = 16

GUARD_MODULE = "test_ios_shader_prescale_guard"
GUARD_PATH = Path(__file__).resolve().parent / "tests" / (GUARD_MODULE + ".py")

# ARMSX2Bridge.mm:2915. The extractor enforces this by silently skipping the entry, so a
# closure with one oversized file installs with that file missing and still passes
# presetCount > 0. Refuse here, where the byte count is already in hand.
MAX_FILE_BYTES = 8 * 1024 * 1024
DEFAULT_CLOSURE_BYTES = 32 * 1024 * 1024

DEFAULT_SAMPLE = 30
# An error in NO_LICENCE runs the safe way: a missed grant costs a preset, not a claim.
DEFAULT_SAMPLE_NO_LICENCE = 10

# A .slangp above this is still config, but the carve-out below is an argument about
# files with no creative content and it should not be asked to carry an arbitrary size.
DEFAULT_CONFIG_BYTES = 4096
QUOTE_CHARS = 160

TEXT_SUFFIXES = {".slangp", ".slang", ".inc", ".h", ".params", ".cgp", ".cg",
                 ".glsl", ".glslp", ".txt", ".md", ".vsh", ".fsh"}
DIRECTORY_LICENCE_NAMES = ("license", "licence", "copying", "license.txt", "licence.txt",
                           "license.md", "copying.txt", "license.zip", "licence.zip")

PUBLIC_DOMAIN = "PUBLIC_DOMAIN"
PERMISSIVE = "PERMISSIVE"
GPL_OR_LATER = "GPL_OR_LATER"
GPL_VERSION_ONLY = "GPL_VERSION_ONLY"
GPL_UNVERSIONED = "GPL_UNVERSIONED"
CONFIG_ONLY = "CONFIG_ONLY"
DIRECTORY_LICENCE = "DIRECTORY_LICENCE"
NO_LICENCE = "NO_LICENCE"
UNCLASSIFIED = "UNCLASSIFIED"

CLASSES = (PUBLIC_DOMAIN, PERMISSIVE, GPL_OR_LATER, GPL_VERSION_ONLY, GPL_UNVERSIONED,
           CONFIG_ONLY, DIRECTORY_LICENCE, NO_LICENCE, UNCLASSIFIED)

LETTER = {
    PUBLIC_DOMAIN: "P", PERMISSIVE: "M", GPL_OR_LATER: "G", GPL_VERSION_ONLY: "Gv",
    GPL_UNVERSIONED: "U", CONFIG_ONLY: "C", DIRECTORY_LICENCE: "L", NO_LICENCE: "N",
    UNCLASSIFIED: "X",
}

CLASS_TEST = {
    PUBLIC_DOMAIN: "the file states public domain or CC0",
    PERMISSIVE: "the full MIT permission block or a complete BSD clause set is present",
    GPL_OR_LATER: "GPL naming a version and permitting later versions",
    GPL_VERSION_ONLY: "GPL naming a version with no 'or later' clause",
    GPL_UNVERSIONED: "a GPL keyword with no version and no resolvable form",
    CONFIG_ONLY: "a header-less .slangp of key = value lines only, under the byte ceiling",
    DIRECTORY_LICENCE: "no per-file keyword, but a LICENSE file governs a parent directory",
    NO_LICENCE: "no licence keyword of any kind",
    UNCLASSIFIED: "conflicting or unrecognised grants, or a pointer to an external file",
}

BUCKETS = ("offered", "unresolved closure", "unclamped prescale", "disallowed extension",
           "file over cap", "closure over cap", "excluded by class")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def load_prescale():
    """Import the detector from the bundled-tree fence rather than copying it.

    The direction is unconventional -- production code importing from a test module --
    and it is deliberate. The alternative is two copies of a regex that decides whether a
    user's frame goes black, or editing a landed SC-10 fence to extract a shared module.
    If the tests directory is ever reorganised, this import moves with it.
    """
    module = sys.modules.get(GUARD_MODULE)
    if module is None:
        spec = importlib.util.spec_from_file_location(GUARD_MODULE, GUARD_PATH)
        module = importlib.util.module_from_spec(spec)
        sys.modules[GUARD_MODULE] = module
        spec.loader.exec_module(module)
    return module.PRESCALE


PRESCALE = load_prescale()


def extractor_extensions(bridge=BRIDGE):
    """The extension allowlist, read out of the extractor instead of restated here."""
    source = bridge.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"ARMSX2IsShaderPackImportName.*?initWithArray:@\[(.*?)\]\]",
                      source, re.S)
    if not match:
        raise SystemExit("cannot read the extension allowlist from %s; the extractor's "
                         "shape changed and this assertion is now blind" % bridge)
    return {"." + name for name in re.findall(r'@"([a-z0-9]+)"', match.group(1))}


class Unresolved(Exception):
    pass


class Unsafe(Exception):
    pass


def is_safe_relative(path):
    """The predicate SkinAssetPath.isSafeRelative applies, so the app's check is a second
    line of defence rather than the only one."""
    trimmed = path.strip()
    if not trimmed:
        return False
    if trimmed.startswith("/") or trimmed.startswith("\\"):
        return False
    if ":" in trimmed:
        return False
    for segment in [s for s in trimmed.split("/") if s]:
        if segment in ("..", "."):
            return False
        if "\\" in segment:
            return False
        for character in segment:
            if ord(character) < 0x20 or ord(character) == 0x7F:
                return False
    return True


class Checkout:
    """The pinned tree, with the reads the resolver repeats held once."""

    def __init__(self, path):
        self.path = Path(path).resolve()
        self._text = {}
        self._stat = {}
        self._listing = {}

    def head(self):
        """Read the checked-out commit without shelling out.

        Only `git apply` may be invoked from this script, so the pin is read out of the
        repository's own files instead of from `git rev-parse`.
        """
        git = self.path / ".git"
        if not git.is_dir():
            raise SystemExit("%s has no .git directory; point --checkout at a clone" % self.path)
        head = (git / "HEAD").read_text(encoding="utf-8").strip()
        if not head.startswith("ref:"):
            return head
        ref = head.split(":", 1)[1].strip()
        loose = git / ref
        if loose.is_file():
            return loose.read_text(encoding="utf-8").strip()
        packed = git / "packed-refs"
        if packed.is_file():
            for line in packed.read_text(encoding="utf-8").splitlines():
                if line.endswith(" " + ref):
                    return line.split(" ", 1)[0].strip()
        raise SystemExit("cannot resolve %s in %s" % (ref, git))

    def names(self, directory):
        if directory not in self._listing:
            full = self.path / directory if directory else self.path
            try:
                self._listing[directory] = {entry.name for entry in full.iterdir()}
            except OSError:
                self._listing[directory] = set()
        return self._listing[directory]

    def exists(self, relative):
        """Exact-case existence, so a run on a case-insensitive filesystem agrees with a
        run on a case-sensitive one."""
        parts = relative.split("/")
        parent = "/".join(parts[:-1])
        if parts[-1] not in self.names(parent):
            return False
        return (self.path / relative).is_file()

    def size(self, relative):
        if relative not in self._stat:
            self._stat[relative] = (self.path / relative).stat().st_size
        return self._stat[relative]

    def data(self, relative):
        return (self.path / relative).read_bytes()

    def text(self, relative):
        if relative not in self._text:
            raw = self.data(relative)
            try:
                self._text[relative] = raw.decode("utf-8")
            except UnicodeDecodeError:
                # Part of this tree is ISO-Latin-1; the Swift library made the same
                # concession and a file that will not decode is still a file to read.
                self._text[relative] = raw.decode("iso-8859-1")
        return self._text[relative]

    def forget(self, relative):
        self._text.pop(relative, None)
        self._stat.pop(relative, None)

    def presets(self):
        found = []
        for current, directories, files in os.walk(self.path):
            directories[:] = sorted(d for d in directories if d != ".git")
            for name in sorted(files):
                if name.endswith(".slangp"):
                    full = Path(current) / name
                    found.append(str(full.relative_to(self.path)))
        return sorted(found)


def strip_components(path, count):
    parts = path.split("/")
    return "/".join(parts[count:])


def patch_targets(text):
    targets = []
    for line in text.splitlines():
        if line.startswith("+++ b/"):
            targets.append(strip_components(line[4:].strip(), 9))
    return targets


def patch_reason(text):
    for line in text.splitlines():
        if line.startswith("diff --git") or line.startswith("---"):
            break
        if line.strip():
            return line.strip()
    return "no reason recorded in the patch"


def shader_patches(directory):
    """Only the patches that touch the bundled preset tree apply to a shader checkout."""
    chosen = []
    # git apply runs inside the checkout, so a relative patch path would resolve there.
    for patch in sorted(Path(directory).resolve().glob("*.patch")):
        text = patch.read_text(encoding="utf-8")
        if "assets/shaders/presets/" in text:
            chosen.append((patch, text))
    return chosen


def git_apply(checkout, arguments):
    completed = subprocess.run(["git", "apply"] + list(arguments), cwd=str(checkout.path),
                               capture_output=True, text=True)
    return completed


def apply_patches(checkout, directory):
    """Patch the tree, then record what changed, per file.

    The zips are the served bytes, so scanning what was built is scanning what is served.
    Serving a patched file makes ARMSX2 a redistributor of MODIFIED third-party work, and
    GPL-2.0-or-later and MIT both require a changed file to be marked as changed, so every
    touched file carries its patch, its reason and both hashes.
    """
    patches = shader_patches(directory)
    if not patches:
        raise SystemExit("no shader patch found in %s. Upstream still turns the frame "
                         "black at 2x and the resolver walks both affected files, so a "
                         "run without the patches would re-ship SC-10." % directory)

    for patch, _ in patches:
        if git_apply(checkout, ["--check", "-R", "-p9", str(patch)]).returncode == 0:
            reverted = git_apply(checkout, ["-R", "-p9", str(patch)])
            if reverted.returncode != 0:
                raise SystemExit("cannot restore %s to upstream: %s"
                                 % (patch.name, reverted.stderr.strip()))

    modified = {}
    for patch, text in patches:
        targets = patch_targets(text)
        before = {}
        for target in targets:
            if not checkout.exists(target):
                raise SystemExit("%s patches %s, which is not in this checkout. The "
                                 "checkout is at the wrong commit; do not drop the patch."
                                 % (patch.name, target))
            checkout.forget(target)
            before[target] = checkout.data(target)

        applied = git_apply(checkout, ["-p9", str(patch)])
        if applied.returncode != 0:
            raise SystemExit("git apply -p9 failed for %s: %s\nThe strip level is right "
                             "for a twelve-component patch path, so the checkout is at "
                             "the wrong commit. Do not fall back to bare upstream: "
                             "upstream still turns the frame black at 2x."
                             % (patch.name, applied.stderr.strip()))

        for target in targets:
            checkout.forget(target)
            served = checkout.data(target)
            modified[target] = {
                "patch": patch.name,
                "reason": patch_reason(text),
                "upstream_bytes": len(before[target]),
                "upstream_sha256": sha256_bytes(before[target]),
                "served_sha256": sha256_bytes(served),
            }
    return modified


def unquote(value):
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1].strip()
    return value


SHADER_KEY = re.compile(r"^shader(\d+)$")
INCLUDE = re.compile(r'^\s*#include\s*[<"]([^>"]+)[>"]')


def preset_facts(text):
    """Pass count, references, stage paths and texture paths, from a .slangp body.

    ShaderPresetLibrary.facts(of:) is a proven parsing idiom and not a resolver: it
    follows #reference alone and stops at the first file declaring a pass count, which is
    all a pass badge needs and less than half of what a closure needs.
    """
    shaders = None
    references = []
    values = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.lower().startswith("#reference"):
            target = unquote(line[len("#reference"):])
            if target:
                references.append(target)
            continue
        if line.startswith("#") or line.startswith("//") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values.setdefault(key.strip().lower(), unquote(value))

    if values.get("shaders", "").isdigit():
        shaders = int(values["shaders"])

    stages = []
    for key, value in values.items():
        match = SHADER_KEY.match(key)
        if match and value:
            stages.append((int(match.group(1)), value))
    stages = [value for _, value in sorted(stages)]

    textures = []
    for name in values.get("textures", "").split(";"):
        name = name.strip().lower()
        if name and values.get(name):
            textures.append(values[name])

    return shaders, references, stages, textures


def includes(text):
    found = []
    for raw in text.splitlines():
        match = INCLUDE.match(raw)
        if match:
            found.append(match.group(1))
    return found


def join_relative(base, target):
    if '"' in target or "'" in target:
        # Upstream ships .slangp values with an unterminated quote. RetroArch's own parser
        # is lenient about it; nothing here knows whether the shipped runtime is, and a
        # preset that resolves to the wrong file compiles to nothing on a user's device.
        raise Unsafe("unterminated quote in %r" % target)
    if not is_safe_relative(target) and not target.startswith("."):
        raise Unsafe(target)
    joined = os.path.normpath(os.path.join(base, target)).replace(os.sep, "/")
    if joined.startswith("..") or joined.startswith("/") or ":" in joined:
        raise Unsafe(target)
    if not is_safe_relative(joined):
        raise Unsafe(target)
    return joined


def resolve_closure(checkout, preset):
    """Every shaderN target, every texture, every #reference and every transitive
    #include, until the set closes.

    A preset whose closure reaches a file that cannot be resolved or read is DROPPED, by
    name. Trimming the closure and shipping the rest produces a preset that fails to
    compile on a user's device, which is the worst failure mode available here.
    """
    files = []
    visited = set()
    passes = [None]

    def add(relative):
        if relative in visited:
            return False
        if not checkout.exists(relative):
            raise Unresolved(relative)
        visited.add(relative)
        files.append(relative)
        return True

    def walk_stage(relative, depth):
        if depth > MAX_DEPTH:
            raise Unresolved("include chain deeper than %d at %s" % (MAX_DEPTH, relative))
        if not add(relative):
            return
        if Path(relative).suffix.lower() not in TEXT_SUFFIXES:
            return
        base = str(Path(relative).parent)
        base = "" if base == "." else base
        for target in includes(checkout.text(relative)):
            walk_stage(join_relative(base, target), depth + 1)

    def walk_preset(relative, depth):
        if depth > MAX_DEPTH:
            raise Unresolved("reference chain deeper than %d at %s" % (MAX_DEPTH, relative))
        if not add(relative):
            return
        base = str(Path(relative).parent)
        base = "" if base == "." else base
        shaders, references, stages, textures = preset_facts(checkout.text(relative))
        if passes[0] is None and shaders is not None:
            passes[0] = shaders
        for stage in stages:
            walk_stage(join_relative(base, stage), depth + 1)
        for texture in textures:
            add(join_relative(base, texture))
        for reference in references:
            walk_preset(join_relative(base, reference), depth + 1)

    walk_preset(preset, 0)
    if not any(Path(f).suffix.lower() != ".slangp" for f in files):
        # A .slangp naming no stage and referencing nothing is a parameter fragment other
        # presets pull in, not a preset. Offering one installs a pack that renders nothing
        # and still passes the installer's presetCount > 0 check.
        raise Unresolved("no shader stage; this .slangp is a parameter fragment")
    return files, passes[0]


def unclamped_prescale(text):
    hits = []
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("//")[0]
        if not PRESCALE.search(code):
            continue
        if "max(" in code or "clamp(" in code:
            continue
        hits.append((number, code.strip()))
    return hits


LICENCE_KEYWORD = re.compile(
    r"licen[cs]e|copyright|public domain|\bGPL\b|\bLGPL\b|General Public License|"
    r"\bMIT\b|\bBSD\b|\bISC\b|Apache|Mozilla|Creative Commons|\bCC0\b|CC-BY|SPDX|"
    r"unlicense|all rights reserved", re.I)
AUTHOR_LINE = re.compile(r"author\s*[:=]|copyright|\bby\s+\w", re.I)
SPDX = re.compile(r"SPDX-License-Identifier\s*:\s*([A-Za-z0-9.\-+]+)", re.I)

PD_TEXT = re.compile(r"public domain|\bCC0\b|\bunlicense\b", re.I)
MIT_NOTICE = re.compile(r"permission is hereby granted", re.I)
BSD_NOTICE = re.compile(r"redistribution and use in source and binary forms", re.I)
NAMED_ONLY = re.compile(r"\bMIT\b|\bBSD\b|\bISC\b", re.I)
GPL_TEXT = re.compile(r"General Public License|\bL?GPL\b|GPL-[0-9]|GPLv[0-9]", re.I)
GPL_VERSION = re.compile(r"version\s*[23](?:\.0)?\b|GPL-?v?\s?[23](?:\.0)?\b", re.I)
GPL_LATER = re.compile(r"any later version|or-later|GPL-?[23](?:\.0)?\+", re.I)
RESTRICTED = re.compile(
    r"creative commons|\bCC-BY\b|share[- ]?alike|non[- ]?commercial|"
    r"Mozilla Public License|\bMPL-|Apache License|Apache-2", re.I)
EXTERNAL_REF = re.compile(
    r"see (?:the )?(?:accompanying )?(?:file )?(?:LICEN[CS]E|COPYING)|"
    r"LICEN[CS]E(?:\.\w+)? file|"
    r"terms (?:in|of) the (?:LICEN[CS]E|COPYING)", re.I)


def licence_evidence(text):
    """What the file states about itself, verbatim, with the line numbers it was read at."""
    quotes = []
    lines = []
    author = ""
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or not LICENCE_KEYWORD.search(line):
            continue
        if len(quotes) < 12:
            quotes.append(line[:QUOTE_CHARS])
            lines.append(number)
        if not author and AUTHOR_LINE.search(line):
            author = line[:QUOTE_CHARS]
    return author, quotes, lines


def spdx_family(text):
    match = SPDX.search(text)
    if not match:
        return None
    token = match.group(1)
    upper = token.upper()
    if upper.startswith("GPL") or upper.startswith("LGPL"):
        if upper.endswith("+") or "OR-LATER" in upper:
            return GPL_OR_LATER
        if re.search(r"[23]", upper):
            return GPL_VERSION_ONLY
        return GPL_UNVERSIONED
    if upper in ("MIT", "ISC") or upper.startswith("BSD"):
        return PERMISSIVE
    if upper in ("CC0-1.0", "UNLICENSE"):
        return PUBLIC_DOMAIN
    return UNCLASSIFIED


def config_only(text, size, ceiling):
    if size > ceiling:
        return False
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("//") or line.startswith(";"):
            continue
        if line.lower().startswith("#reference"):
            continue
        # A leading # is a comment in this format. #include is not: it pulls source into
        # the file and takes the carve-out's argument with it.
        if line.lower().startswith("#include") or line.lower().startswith("#pragma"):
            return False
        if line.startswith("#"):
            continue
        if "=" not in line:
            return False
    return True


def config_ceiling_spread(checkout, records, ceilings):
    """What the stated byte ceiling costs, so the human signs a number rather than
    inherits one."""
    spread = []
    no_family = (CONFIG_ONLY, DIRECTORY_LICENCE, NO_LICENCE)
    presets = [r for r in records.values()
               if r.path.endswith(".slangp") and r.cls in no_family]
    for ceiling in ceilings:
        count = sum(1 for record in presets
                    if config_only(checkout.text(record.path), record.bytes, ceiling))
        spread.append((ceiling, count))
    return spread


def directory_licence(checkout, relative):
    parent = Path(relative).parent
    while str(parent) not in (".", "/"):
        for name in checkout.names(str(parent)):
            if name.lower() in DIRECTORY_LICENCE_NAMES:
                return str(parent) + "/" + name
        parent = parent.parent
    return ""


def classify_file(checkout, relative, text, size, ceiling):
    """Assign an evidence class from quoted text, and never a verdict.

    A class is a statement about evidence: this header states GPL with an "or later"
    clause at lines 68-73. A verdict is a statement about redistributability. Only the
    signed rules map one to the other, and `emit` applies that map mechanically.
    """
    families = set()
    spdx = spdx_family(text)
    if spdx:
        families.add(spdx)
    if PD_TEXT.search(text):
        families.add(PUBLIC_DOMAIN)
    if MIT_NOTICE.search(text) or BSD_NOTICE.search(text):
        families.add(PERMISSIVE)
    if GPL_TEXT.search(text):
        if GPL_LATER.search(text):
            families.add(GPL_OR_LATER)
        elif GPL_VERSION.search(text):
            families.add(GPL_VERSION_ONLY)
        else:
            families.add(GPL_UNVERSIONED)
    if RESTRICTED.search(text):
        families.add(UNCLASSIFIED)
    # A licence named with no grant behind it, or a pointer to a file this scanner is not
    # allowed to read across, resolves nothing on its own. Alongside a grant it is a
    # second statement, and two statements in one file are what the >1 test already
    # catches, so neither test is allowed to overrule a grant that is actually present.
    if not families and (EXTERNAL_REF.search(text) or NAMED_ONLY.search(text)):
        families.add(UNCLASSIFIED)

    if UNCLASSIFIED in families or len(families) > 1:
        return UNCLASSIFIED, directory_licence(checkout, relative)
    if families:
        return families.pop(), directory_licence(checkout, relative)

    # Nothing in the file. The carve-out below is Plan 03's own, made machine-checkable:
    # not one .slangp in any closure carries a header, so the standing rule read strictly
    # rejects the whole collection. The ceiling and the no-expression test are what keep
    # it from drifting into source -- a header-less eight-line #define wrapper is a .slang
    # and falls to NO_LICENCE, even though it is bundled today.
    governing = directory_licence(checkout, relative)
    if Path(relative).suffix.lower() == ".slangp" and config_only(text, size, ceiling):
        return CONFIG_ONLY, governing
    if governing:
        return DIRECTORY_LICENCE, governing
    return NO_LICENCE, governing


class FileRecord:
    def __init__(self, relative):
        self.path = relative
        self.bytes = 0
        self.sha256 = ""
        self.upstream_bytes = 0
        self.author = ""
        self.quotes = []
        self.lines = []
        self.cls = NO_LICENCE
        self.governing = ""
        self.prescale = []
        self.modified = None

    def quoted(self):
        return " | ".join(self.quotes)

    def read_at(self):
        return ",".join(str(n) for n in self.lines)


def describe_files(checkout, paths, modified, ceiling):
    records = {}
    for relative in sorted(paths):
        record = FileRecord(relative)
        data = checkout.data(relative)
        record.bytes = len(data)
        record.sha256 = sha256_bytes(data)
        record.upstream_bytes = record.bytes
        record.modified = modified.get(relative)
        if record.modified:
            record.upstream_bytes = record.modified["upstream_bytes"]
        if Path(relative).suffix.lower() in TEXT_SUFFIXES:
            text = checkout.text(relative)
            record.author, record.quotes, record.lines = licence_evidence(text)
            record.cls, record.governing = classify_file(
                checkout, relative, text, record.bytes, ceiling)
            record.prescale = unclamped_prescale(text)
        else:
            record.cls, record.governing = classify_file(
                checkout, relative, "", record.bytes, ceiling)
        records[relative] = record
    return records


class Entry:
    def __init__(self, preset, files, passes):
        self.preset = preset
        self.files = files
        self.passes = passes
        self.identifier = preset[:-len(".slangp")]
        self.name = Path(preset).stem
        parts = preset.split("/")
        self.category = parts[0] if len(parts) > 1 else ""
        self.bucket = "offered"
        self.detail = ""


def resolve_all(checkout, allowed_extensions, closure_cap):
    """Every .slangp in the tree lands in exactly one bucket.

    At sixteen presets a silent drop is visible. At two thousand it is not, and a resolver
    bug then looks exactly like a licence exclusion, so the buckets must sum to the tree's
    total .slangp count.
    """
    entries = []
    for preset in checkout.presets():
        entry = Entry(preset, [], None)
        try:
            entry.files, entry.passes = resolve_closure(checkout, preset)
        except Unresolved as problem:
            entry.bucket, entry.detail = "unresolved closure", str(problem)
            entries.append(entry)
            continue
        except Unsafe as problem:
            entry.bucket, entry.detail = "unresolved closure", "unsafe path %s" % problem
            entries.append(entry)
            continue
        except (OSError, RecursionError) as problem:
            entry.bucket, entry.detail = "unresolved closure", str(problem)
            entries.append(entry)
            continue

        bad = [f for f in entry.files if Path(f).suffix.lower() not in allowed_extensions]
        if bad:
            entry.bucket, entry.detail = "disallowed extension", bad[0]
            entries.append(entry)
            continue

        oversized = [f for f in entry.files if checkout.size(f) > MAX_FILE_BYTES]
        if oversized:
            entry.bucket, entry.detail = "file over cap", oversized[0]
            entries.append(entry)
            continue

        total = sum(checkout.size(f) for f in entry.files)
        if total > closure_cap:
            entry.bucket, entry.detail = "closure over cap", "%d bytes" % total
            entries.append(entry)
            continue

        entries.append(entry)
    return entries


def apply_prescale_gate(entries, records):
    """Refuse rather than auto-patch.

    A generator that invented a clamp would be modifying third-party source with nobody's
    name on the change. If the repo's patches cover the file the scan is already clean,
    because the scan runs over the patched tree.
    """
    offenders = {}
    for entry in entries:
        if entry.bucket != "offered":
            continue
        for relative in entry.files:
            record = records.get(relative)
            if record and record.prescale:
                offenders.setdefault(relative, record.prescale)
                entry.bucket = "unclamped prescale"
                entry.detail = "%s:%d" % (relative, record.prescale[0][0])
                break
    return offenders


STANDING_INCLUDE = (PUBLIC_DOMAIN, PERMISSIVE, GPL_OR_LATER, CONFIG_ONLY)
OPEN_CLASSES = (GPL_VERSION_ONLY, GPL_UNVERSIONED, DIRECTORY_LICENCE)


def survivors(entries, records, admitted):
    return sum(1 for entry in entries if entry.bucket == "offered"
               and all(records[f].cls in admitted for f in entry.files))


def admission_spread(entries, records):
    """What each of the three open classes is worth, in presets.

    The six standing answers are held fixed. This says nothing about which way to decide;
    it says what the decision costs, which is the one figure the run cannot be asked for
    afterwards.
    """
    rows = []
    for mask in range(1 << len(OPEN_CLASSES)):
        chosen = [cls for bit, cls in enumerate(OPEN_CLASSES) if mask & (1 << bit)]
        admitted = set(STANDING_INCLUDE) | set(chosen)
        rows.append(tuple(["yes" if cls in chosen else "no" for cls in OPEN_CLASSES]
                          + [survivors(entries, records, admitted)]))
    return sorted(rows, key=lambda row: row[-1])


def sample_rows(pin, cls, paths, size):
    """Ranked by sha256(pin + class + path), so a second reader recomputes the same rows
    with a one-liner. A sample nobody else can reproduce is an anecdote."""
    ordered = sorted(paths)
    ranked = sorted(ordered, key=lambda p: sha256_bytes((pin + cls + p).encode("utf-8")))
    return ranked[:size]


def read_rules(path, pin):
    """`emit` refuses without a signed rules file at this pin, so the manifest cannot
    physically exist before the sign-off. There is no flag that skips this."""
    if not path:
        raise SystemExit("emit needs --rules. The licence sign-off is the gate: without a "
                         "signed rules file there is nothing to fill a verdict from.")
    source = Path(path)
    if not source.is_file():
        raise SystemExit("no rules file at %s" % source)
    text = source.read_text(encoding="utf-8")

    def field(name):
        match = re.search(r"^\s*%s\s*:\s*(.+)$" % name, text, re.M | re.I)
        return match.group(1).strip() if match else ""

    signer = field("Signed-by")
    signed_on = field("Signed-on")
    placeholder = ("", "tbd", "todo", "unsigned", "-", "none", "name")
    if signer.lower() in placeholder or signed_on.lower() in placeholder:
        raise SystemExit("%s is not signed: Signed-by and Signed-on must both carry a "
                         "real value. A rules file nobody signed is not a gate." % source)

    recorded = field("Pin")
    if recorded != pin:
        raise SystemExit("%s records pin %r but the checkout is at %r"
                         % (source, recorded, pin))

    decisions = {}
    overrides = {}
    for line in text.splitlines():
        if not line.strip().startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        verdicts = [c.lower() for c in cells if c.lower() in ("include", "exclude")]
        if not verdicts:
            continue
        named = [c for c in cells if c in CLASSES]
        if named:
            decisions[named[0]] = verdicts[0]
            continue
        paths = [c for c in cells if "/" in c and is_safe_relative(c)]
        if paths:
            overrides[paths[0]] = verdicts[0]

    return {"path": str(source), "sha256": sha256_bytes(text.encode("utf-8")),
            "signer": signer, "signed_on": signed_on,
            "decisions": decisions, "overrides": overrides}


def signed_verdict(record, rules):
    override = rules["overrides"].get(record.path)
    if override:
        return override
    return rules["decisions"].get(record.cls, "")


def zip_entry(relative):
    info = zipfile.ZipInfo(relative)
    # mtimes come from the clone, so they must not reach the archive: without this a
    # fresh checkout on another machine produces different zip bytes, a different zip
    # SHA-256 and therefore a different manifest, and the reviewable diff is a fiction.
    info.date_time = (1980, 1, 1, 0, 0, 0)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    info.create_system = 3
    return info


def write_zip(target, checkout, files):
    target.parent.mkdir(parents=True, exist_ok=True)
    payload = {relative: checkout.data(relative) for relative in files}
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative in sorted(payload):
            archive.writestr(zip_entry(relative), payload[relative])
    return payload


def verify_zip(target, payload, declared):
    """zip member = manifest hash = patched-checkout file, asserted from the same read
    that archived the byte. It is what stops the manifest asserting clean while serving
    something else."""
    with zipfile.ZipFile(target) as archive:
        members = sorted(archive.namelist())
        if members != sorted(payload):
            raise SystemExit("%s does not contain its closure" % target)
        for member in members:
            data = archive.read(member)
            if data != payload[member]:
                raise SystemExit("%s: %s is not the byte that was scanned" % (target, member))
            if sha256_bytes(data) != declared[member]:
                raise SystemExit("%s: %s does not match the manifest hash" % (target, member))


AUDIT_CLOSURES = {
    "crt/zfast-crt.slangp": (3, 3993),
    "crt/crt-hyllian-fast.slangp": (2, 5269),
    "crt/crt-geom.slangp": (2, 30839),
    "scanlines/scanlines-sine-abs.slangp": (2, 1812),
    "scanlines/res-independent-scanlines.slangp": (3, 26355),
    "handheld/lcd3x.slangp": (2, 1537),
    "handheld/sameboy-lcd.slangp": (2, 5587),
    "pixel-art-scaling/sharp-bilinear.slangp": (2, 1825),
    "pixel-art-scaling/sharp-bilinear-simple.slangp": (2, 2524),
    "crt/crt-lottes.slangp": (2, 9634),
    "crt/crt-pi.slangp": (2, 7612),
    "crt/crt-caligari.slangp": (2, 4458),
    "crt/crt-cgwg-fast.slangp": (2, 4566),
    "handheld/lcd1x.slangp": (2, 2234),
}
AUDIT_UNAUDITED_CLOSURES = ("crt/crt-aperture.slangp", "crt/crt-easymode.slangp")


def crosscheck(entries, records):
    """The sixteen presets audited on 2026-08-16 are the one check here with a
    known-correct answer. A disagreement is a resolver or classifier bug."""
    rows = []
    by_preset = {entry.preset: entry for entry in entries}
    for preset, (count, size) in sorted(AUDIT_CLOSURES.items()):
        entry = by_preset.get(preset)
        if entry is None or not entry.files:
            rows.append((preset, "MISSING", "", "", "", "resolver produced no closure"))
            continue
        upstream = sum(records[f].upstream_bytes for f in entry.files if f in records)
        classes = ",".join(sorted({LETTER[records[f].cls] for f in entry.files
                                   if f in records}))
        agree = "ok" if (len(entry.files) == count and upstream == size) else "DIFFERS"
        rows.append((preset, agree, count, len(entry.files), "%d/%d" % (size, upstream),
                     classes))
    for preset in AUDIT_UNAUDITED_CLOSURES:
        entry = by_preset.get(preset)
        if entry is None:
            rows.append((preset, "MISSING", "", "", "", ""))
            continue
        classes = ",".join(sorted({LETTER[records[f].cls] for f in entry.files
                                   if f in records}))
        rows.append((preset, "no audit figure", "", len(entry.files), "", classes))
    return rows


def table(header, rows):
    lines = ["| " + " | ".join(header) + " |",
             "|" + "|".join(["---"] * len(header)) + "|"]
    for row in rows:
        lines.append("| " + " | ".join(str(cell) for cell in row) + " |")
    return "\n".join(lines)


def reconciliation(entries, total):
    counts = {bucket: 0 for bucket in BUCKETS}
    for entry in entries:
        counts[entry.bucket] = counts.get(entry.bucket, 0) + 1
    summed = sum(counts.values())
    if summed != total:
        raise SystemExit("reconciliation does not close: %d bucketed against %d presets "
                         "in the tree" % (summed, total))
    return counts


def write_ballot(out, checkout, pin, entries, records, counts, offenders, options):
    populations = {cls: sorted(r.path for r in records.values() if r.cls == cls)
                   for cls in CLASSES}
    reachable = {cls: 0 for cls in CLASSES}
    for entry in entries:
        if entry.bucket != "offered":
            continue
        for cls in {records[f].cls for f in entry.files if f in records}:
            reachable[cls] += 1

    samples = {}
    for cls in CLASSES:
        size = options["sample_no_licence"] if cls == NO_LICENCE else options["sample"]
        samples[cls] = sample_rows(pin, cls, populations[cls], size)

    parts = []
    parts.append("# Catalogue licence classes -- DRAFT ballot\n")
    parts.append("Generated by shader_manifest.py at pin `%s`. This file lives in the "
                 "generated build directory and a re-run overwrites it. The signed copy "
                 "belongs in the tracked tree at "
                 "platforms/ios/patches/CATALOGUE-LICENCE-VERDICTS.md.\n" % pin)
    parts.append("Evidence table: licence-evidence.csv, one row per file in a resolved "
                 "closure, quoted verbatim and truncated to %d characters.\n" % QUOTE_CHARS)

    parts.append("\n## Reconciliation\n")
    parts.append(table(["Bucket", "Presets"],
                       [(b, counts.get(b, 0)) for b in BUCKETS]))
    parts.append("\nTotal .slangp in the tree: %d.\n" % len(entries))

    parts.append("\n## Class populations at this pin\n")
    parts.append(table(["Class", "Code", "Test on the file's own text", "Files",
                        "Candidate presets reachable"],
                       [(LETTER[c], c, CLASS_TEST[c], len(populations[c]), reachable[c])
                        for c in CLASSES]))
    parts.append("\n\"Candidate presets reachable\" counts presets that pass every "
                 "non-licence rule and carry at least one file of that class, so "
                 "excluding the class removes them.\n")

    parts.append("\n## What the %s byte ceiling costs\n" % CONFIG_ONLY)
    parts.append("This run used %d bytes. The ceiling is a signed parameter: a header-less "
                 ".slangp above it falls to %s or %s instead. Re-run classify with "
                 "--config-max-bytes to move it.\n"
                 % (options["config_bytes"], DIRECTORY_LICENCE, NO_LICENCE))
    parts.append(table(["Ceiling, bytes", "Header-less .slangp that qualify"],
                       config_ceiling_spread(checkout, records,
                                             (1024, 2048, 4096, 8192, 16384, 1 << 30))))

    parts.append("\n## What the three open classes are worth\n")
    parts.append("Presets that survive with %s held included, %s and %s held excluded, "
                 "under each way the three open classes could go. How many presets the "
                 "run offers is an output of the rules, so this table is the cost of the "
                 "decision and not an argument for either side of it.\n"
                 % (", ".join(LETTER[c] for c in STANDING_INCLUDE),
                    LETTER[NO_LICENCE], LETTER[UNCLASSIFIED]))
    parts.append(table([LETTER[c] + " admitted" for c in OPEN_CLASSES] + ["Presets"],
                       admission_spread(entries, records)))

    parts.append("\n## Signed class rules\n")
    parts.append("Fill Decision with `include` or `exclude` and give a one-line reason. "
                 "A class with a non-zero population and no decision makes `emit` "
                 "refuse.\n")
    parts.append(table(["Class", "Code", "Files", "Decision", "Reason"],
                       [(LETTER[c], c, len(populations[c]), "", "") for c in CLASSES]))

    parts.append("\n## Per-file overrides\n")
    parts.append("Admitting generalises, so it is a class decision. Refusing does not "
                 "have to generalise, so it stays a file decision: strike any row in any "
                 "admitted class without justifying it. Per-file admission belongs only "
                 "in %s, which is read in full.\n" % UNCLASSIFIED)
    parts.append(table(["Path", "Decision", "Reason"], [("", "", "")]))

    for cls in CLASSES:
        if not populations[cls]:
            continue
        size = options["sample_no_licence"] if cls == NO_LICENCE else options["sample"]
        parts.append("\n## Sample -- %s (%s), %d of %d rows\n"
                     % (LETTER[cls], cls, len(samples[cls]), len(populations[cls])))
        parts.append("Ranked by sha256(pin + class + path). Zero misclassifications or "
                     "the class is not signed; one bad row means the classifier is "
                     "wrong, not that one row is wrong. Sample size %d bounds this "
                     "class at about %.0f%% by the rule of three.\n"
                     % (size, 300.0 / max(size, 1)))
        parts.append(table(["Path", "Read at", "Stated"],
                           [(p, records[p].read_at() or "-",
                             (records[p].quoted() or "-").replace("|", "/"))
                            for p in samples[cls]]))

    parts.append("\n## Class %s in full\n" % UNCLASSIFIED)
    parts.append("The classifier gave up on these. Read every row and decide each one.\n")
    parts.append(table(["Path", "Read at", "Stated"],
                       [(p, records[p].read_at() or "-",
                         (records[p].quoted() or "-").replace("|", "/"))
                        for p in populations[UNCLASSIFIED]] or [("none", "", "")]))

    parts.append("\n## Unclamped prescale, in full\n")
    parts.append("Every file below carries the SC-10 bug with no patch behind it. Extend "
                 "platforms/ios/patches/ -- which is a new modification needing its own "
                 "sign-off -- or leave the presets refused.\n")
    parts.append(table(["Path", "Line", "Source"],
                       [(path, hits[0][0], hits[0][1].replace("|", "/"))
                        for path, hits in sorted(offenders.items())] or [("none", "", "")]))

    parts.append("\n## Presets dropped, by name\n")
    parts.append("Every row is also in dropped-presets.csv. A resolver bug and a licence "
                 "exclusion look identical from the outside unless the buckets are "
                 "separated, which is what this list is for.\n")
    for bucket in BUCKETS:
        if bucket == "offered":
            continue
        dropped = [(e.preset, e.detail) for e in entries if e.bucket == bucket]
        if not dropped:
            continue
        parts.append("\n### %s -- %d\n" % (bucket, len(dropped)))
        shown = sorted(dropped)[:60]
        parts.append(table(["Preset", "Why"], shown))
        if len(dropped) > len(shown):
            parts.append("\n%d more in dropped-presets.csv.\n" % (len(dropped) - len(shown)))

    parts.append("\n## Cross-check against the bundled-preset audit\n")
    parts.append(table(["Preset", "Agrees", "Audit files", "Resolved files",
                        "Audit/resolved upstream bytes", "Classes"],
                       crosscheck(entries, records)))

    parts.append("\n## Signature\n")
    parts.append("    Signed-by: TBD\n    Signed-on: TBD\n    Pin: %s\n"
                 "    Sample-size: %d\n    Sample-size-%s: %d\n"
                 "    Evidence-SHA256: TBD\n"
                 % (pin, options["sample"], NO_LICENCE, options["sample_no_licence"]))

    text = "\n".join(parts) + "\n"
    (out / "LICENCE-CLASSES-DRAFT.md").write_text(text, encoding="utf-8")
    return populations


EVIDENCE_HEADER = ["path", "bytes", "sha256", "author_as_stated", "licence_as_stated",
                   "read_at_lines", "class", "class_code", "governing_licence_file",
                   "modified_patch", "modified_reason", "upstream_sha256",
                   "served_sha256", "verdict"]


def write_evidence(target, records, rules=None):
    with open(target, "w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(EVIDENCE_HEADER)
        for path in sorted(records):
            record = records[path]
            modified = record.modified or {}
            writer.writerow([
                record.path, record.bytes, record.sha256, record.author,
                record.quoted(), record.read_at(), record.cls, LETTER[record.cls],
                record.governing, modified.get("patch", ""), modified.get("reason", ""),
                modified.get("upstream_sha256", ""), modified.get("served_sha256", ""),
                signed_verdict(record, rules) if rules else "",
            ])
    return sha256_bytes(target.read_bytes())


def write_dropped(target, entries):
    with open(target, "w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["preset", "bucket", "why"])
        for entry in sorted(entries, key=lambda e: (e.bucket, e.preset)):
            if entry.bucket != "offered":
                writer.writerow([entry.preset, entry.bucket, entry.detail])


def build(argv, command):
    parser = argparse.ArgumentParser(prog="shader_manifest.py " + command)
    parser.add_argument("--checkout", required=True,
                        help="a slang-shaders clone at pin " + PIN)
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument("--patches", default=str(DEFAULT_PATCHES))
    parser.add_argument("--sample", type=int, default=DEFAULT_SAMPLE)
    parser.add_argument("--sample-no-licence", type=int, default=DEFAULT_SAMPLE_NO_LICENCE)
    parser.add_argument("--config-max-bytes", type=int, default=DEFAULT_CONFIG_BYTES)
    parser.add_argument("--max-closure-bytes", type=int, default=DEFAULT_CLOSURE_BYTES)
    parser.add_argument("--generated-at", default="")
    parser.add_argument("--bridge", default=str(BRIDGE))
    if command == "emit":
        parser.add_argument("--rules", default="")
    return parser.parse_args(argv)


def open_checkout(options):
    checkout = Checkout(options.checkout)
    pin = checkout.head()
    if pin != PIN:
        raise SystemExit("the checkout is at %s but this generator only runs at %s. The "
                         "patch context, the cross-check and the audit's byte counts all "
                         "hold only at that pin." % (pin, PIN))
    return checkout, pin


def prepare(options):
    checkout, pin = open_checkout(options)
    out = Path(options.out)
    out.mkdir(parents=True, exist_ok=True)

    modified = apply_patches(checkout, options.patches)
    allowed = extractor_extensions(Path(options.bridge))
    entries = resolve_all(checkout, allowed, options.max_closure_bytes)

    wanted = set()
    for entry in entries:
        if entry.bucket in ("offered", "unclamped prescale"):
            wanted.update(entry.files)
    records = describe_files(checkout, wanted, modified, options.config_max_bytes)
    offenders = apply_prescale_gate(entries, records)
    return checkout, pin, out, entries, records, offenders, modified


def print_reconciliation(counts, total):
    print("reconciliation, %d .slangp in the tree:" % total)
    for bucket in BUCKETS:
        print("  %-22s %6d" % (bucket, counts.get(bucket, 0)))


def command_classify(argv):
    options = build(argv, "classify")
    checkout, pin, out, entries, records, offenders, _ = prepare(options)
    counts = reconciliation(entries, len(entries))

    settings = {"sample": options.sample,
                "sample_no_licence": options.sample_no_licence,
                "config_bytes": options.config_max_bytes}
    write_ballot(out, checkout, pin, entries, records, counts, offenders, settings)
    write_dropped(out / "dropped-presets.csv", entries)
    digest = write_evidence(out / "licence-evidence.csv", records)

    print_reconciliation(counts, len(entries))
    print("files in resolved closures: %d" % len(records))
    for cls in CLASSES:
        print("  %-2s %-18s %6d" % (LETTER[cls], cls,
                                    sum(1 for r in records.values() if r.cls == cls)))
    print("unclamped prescale, distinct files: %d" % len(offenders))
    print("ballot:   %s" % (out / "LICENCE-CLASSES-DRAFT.md"))
    print("evidence: %s  sha256 %s" % (out / "licence-evidence.csv", digest))
    print("no manifest and no zip were written: emit needs a signed rules file")
    return 0


def command_emit(argv):
    options = build(argv, "emit")
    # The gate comes before the work, so a refusal cannot leave half a catalogue behind.
    rules = read_rules(options.rules, open_checkout(options)[1])
    checkout, pin, out, entries, records, offenders, modified = prepare(options)

    populations = {cls: sum(1 for r in records.values() if r.cls == cls) for cls in CLASSES}
    undecided = [cls for cls in CLASSES
                 if populations[cls] and cls not in rules["decisions"]]
    if undecided:
        raise SystemExit("no decision in %s for: %s. Every class with a population needs "
                         "a rule before a verdict can be filled from it."
                         % (rules["path"], ", ".join(undecided)))

    for entry in entries:
        if entry.bucket != "offered":
            continue
        refused = [f for f in entry.files
                   if signed_verdict(records[f], rules) != "include"]
        if refused:
            entry.bucket = "excluded by class"
            entry.detail = "%s (%s)" % (refused[0], LETTER[records[refused[0]].cls])

    counts = reconciliation(entries, len(entries))
    write_dropped(out / "dropped-presets.csv", entries)
    offered = [entry for entry in entries if entry.bucket == "offered"]

    generated = options.generated_at or (
        datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT00:00:00Z"))
    manifest = {
        "schema": 1,
        "generator": "platforms/ios/scripts/shader_manifest.py",
        "pin": pin,
        "patches": sorted({record["patch"] for record in modified.values()}),
        "rules": os.path.basename(rules["path"]),
        "rules_sha256": rules["sha256"],
        "generated": generated,
        "max_file_bytes": MAX_FILE_BYTES,
        "max_closure_bytes": options.max_closure_bytes,
        "entries": [],
    }

    zips = out / "zips"
    for entry in sorted(offered, key=lambda e: e.identifier):
        files = []
        declared = {}
        for relative in entry.files:
            record = records[relative]
            if not is_safe_relative(relative):
                raise SystemExit("%s is not a safe relative path" % relative)
            payload = {"path": relative, "bytes": record.bytes, "sha256": record.sha256}
            payload["modified"] = None
            if record.modified:
                payload["modified"] = {
                    "patch": record.modified["patch"],
                    "reason": record.modified["reason"],
                    "upstream_sha256": record.modified["upstream_sha256"],
                    "served_sha256": record.modified["served_sha256"],
                }
            files.append(payload)
            declared[relative] = record.sha256

        archive = zips / (entry.identifier + ".zip")
        written = write_zip(archive, checkout, entry.files)
        verify_zip(archive, written, declared)
        data = archive.read_bytes()
        relative_zip = str(archive.relative_to(out))
        if not is_safe_relative(relative_zip):
            raise SystemExit("%s is not a safe relative path" % relative_zip)

        manifest["entries"].append({
            "id": entry.identifier,
            "name": entry.name,
            "category": entry.category,
            "preset": entry.preset,
            "passes": entry.passes,
            "closure_bytes": sum(f["bytes"] for f in files),
            "files": files,
            "zip": {"path": relative_zip, "bytes": len(data),
                    "sha256": sha256_bytes(data)},
        })

    body = (json.dumps(manifest, indent=2, ensure_ascii=True) + "\n").encode("utf-8")
    (out / "manifest.json").write_bytes(body)
    compressed = len(gzip.compress(body, 9, mtime=0))
    digest = write_evidence(out / "licence-evidence.csv", records, rules)

    print_reconciliation(counts, len(entries))
    print("offered: %d presets, %d files" % (len(offered), len(records)))
    print("manifest: %s  %d bytes raw, %d bytes gzipped"
          % (out / "manifest.json", len(body), compressed))
    print("evidence: %s" % (out / "licence-evidence.csv"))
    print("evidence sha256, paste into the signed document: %s" % digest)
    print("signed by %s on %s, rules sha256 %s"
          % (rules["signer"], rules["signed_on"], rules["sha256"]))
    return 0


COMMANDS = {"classify": command_classify, "emit": command_emit}


def main(argv):
    if not argv or argv[0] not in COMMANDS:
        print(__doc__.strip())
        return 2
    sys.setrecursionlimit(10000)
    return COMMANDS[argv[0]](argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
