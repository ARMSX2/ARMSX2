#!/usr/bin/env python3
"""The catalogue generator must not serve a preset nobody looked at.

shader_manifest.py resolves preset closures off-device and describes them for a manifest
an app will fetch. Three of its rules are load-bearing and none of them is visible in the
output when it is working: the closure is complete or the preset is dropped, the bytes in
the zip are the bytes that were scanned and hashed, and no manifest exists before a person
signed the class rules it was built from.

Everything here runs against a synthetic tree in a temp directory. No network, no clone.
"""

import hashlib
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
GENERATOR = ROOT / "platforms/ios/scripts/shader_manifest.py"
BRIDGE = ROOT / "platforms/ios/app/src/main/cpp/ARMSX2Bridge.mm"
REAL_PATCH = ROOT / "platforms/ios/patches/slang-shaders-prescale-zero-guard.patch"


def load(name, path):
    module = sys.modules.get(name)
    if module is None:
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return module


manifest = load("shader_manifest", GENERATOR)
guard = load("test_ios_shader_prescale_guard",
             Path(__file__).resolve().parent / "test_ios_shader_prescale_guard.py")

# The two spellings the SC-10 patch turns into each other, copied out of the patch itself
# so the fence tests the actual bug rather than a paraphrase of it.
UNCLAMPED = "    float scale = floor(params.OutputSize.y * params.SourceSize.w);"
CLAMPED = "    float scale = max(floor(params.OutputSize.y * params.SourceSize.w), 1.0);"

GPL_OR_LATER_HEADER = """/*
   Copyright (C) 2011 Somebody
   This program is free software; you can redistribute it and/or modify it under the
   terms of the GNU General Public License as published by the Free Software Foundation;
   either version 2 of the License, or (at your option) any later version.
*/
"""
GPL_VERSION_ONLY_HEADER = """/*
   Copyright (C) 2011 Somebody
   Licensed under the GNU General Public License, version 2.
*/
"""
GPL_UNVERSIONED_HEADER = """/*
    CRT Shader by EasyMode
    License: GPL
*/
"""
MIT_HEADER = """/*
   Copyright (c) 2015 Somebody
   Permission is hereby granted, free of charge, to any person obtaining a copy of this
   software and associated documentation files, to deal in the Software without
   restriction.
*/
"""
PUBLIC_DOMAIN_HEADER = """// author: hunterk
// license: public domain
"""
CONFLICTING_HEADER = """/*
   Copyright (C) 2011 Somebody
   Released into the public domain by the original author.
   This port is under the GNU General Public License, either version 2 or, at your
   option, any later version.
*/
"""
BODY = """
#version 450
void main() { }
"""


def stage(header, extra=""):
    return header + BODY + extra


class Tree:
    """A synthetic checkout: a preset tree, a .git/HEAD at the pin, and a patch directory."""

    def __init__(self):
        self.base = Path(tempfile.mkdtemp(prefix="shader-manifest-fence-"))
        self.checkout = self.base / "checkout"
        self.patches = self.base / "patches"
        self.out = self.base / "out"
        for directory in (self.checkout, self.patches, self.out):
            directory.mkdir(parents=True)
        self.head(manifest.PIN)

    def head(self, pin):
        git = self.checkout / ".git"
        git.mkdir(exist_ok=True)
        (git / "HEAD").write_text(pin + "\n", encoding="utf-8")

    def write(self, relative, text):
        target = self.checkout / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(text, bytes):
            target.write_bytes(text)
        else:
            target.write_text(text, encoding="utf-8")
        return target

    def patch(self, name, body):
        (self.patches / name).write_text(body, encoding="utf-8")

    def drop(self):
        shutil.rmtree(self.base, ignore_errors=True)


def prescale_patch(relative, before, after):
    """A patch shaped like the repo's own: twelve components deep, so -p9 is required."""
    head = "a/platforms/ios/app/src/main/assets/shaders/presets/" + relative
    tail = "b/platforms/ios/app/src/main/assets/shaders/presets/" + relative
    return (
        "Clamp a derived prescale so the frame does not go black above 1.5x.\n"
        "\n"
        "diff --git %s %s\n--- %s\n+++ %s\n" % (head, tail, head, tail) +
        "@@ -1,3 +1,3 @@\n"
        " #version 450\n"
        "-%s\n" % before +
        "+%s\n" % after +
        " void main() { }\n")


def patchable_stage(before):
    return "#version 450\n%s\nvoid main() { }\n" % before


def run(command, tree, extra=(), out=None):
    argv = [command, "--checkout", str(tree.checkout),
            "--out", str(out or tree.out),
            "--patches", str(tree.patches),
            "--bridge", str(BRIDGE)]
    argv.extend(extra)
    sink = io.StringIO()
    with redirect_stdout(sink):
        code = manifest.main(argv)
    return code, sink.getvalue()


def rules_file(tree, decisions, pin=None, signer="A Developer", name="rules.md"):
    rows = ["| %s | %s | %s | signed by hand |" % (manifest.LETTER[cls], cls, decision)
            for cls, decision in decisions.items()]
    body = ("# Catalogue licence verdicts\n\n"
            "| Class | Code | Decision | Reason |\n|---|---|---|---|\n"
            + "\n".join(rows) + "\n\n"
            "    Signed-by: %s\n"
            "    Signed-on: 2026-08-18\n"
            "    Pin: %s\n" % (signer, pin or manifest.PIN))
    target = tree.base / name
    target.write_text(body, encoding="utf-8")
    return target


ADMIT_ALL = {cls: "include" for cls in manifest.CLASSES}


class Closure(unittest.TestCase):
    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.drop)

    def resolve(self, preset):
        checkout = manifest.Checkout(self.tree.checkout)
        return manifest.resolve_closure(checkout, preset)

    def test_a_stage_reaching_a_header_two_directories_up_is_in_the_closure(self):
        self.tree.write("scanlines/res.slangp", "shaders = 1\nshader0 = shaders/res.slang\n")
        self.tree.write("scanlines/shaders/res.slang",
                        '#include "../../include/masks.h"\n' + stage(PUBLIC_DOMAIN_HEADER))
        self.tree.write("include/masks.h", stage(PUBLIC_DOMAIN_HEADER))

        files, passes = self.resolve("scanlines/res.slangp")
        self.assertEqual(files, ["scanlines/res.slangp", "scanlines/shaders/res.slang",
                                 "include/masks.h"])
        self.assertEqual(passes, 1)
        for relative in files:
            self.assertTrue(manifest.is_safe_relative(relative), relative)

    def test_a_reference_cycle_terminates(self):
        self.tree.write("a.slangp", "#reference b.slangp\n")
        self.tree.write("b.slangp", "#reference a.slangp\nshaders = 1\nshader0 = b.slang\n")
        self.tree.write("b.slang", stage(MIT_HEADER))
        files, passes = self.resolve("a.slangp")
        self.assertEqual(files, ["a.slangp", "b.slangp", "b.slang"])
        self.assertEqual(passes, 1)

    def test_a_cycle_that_never_reaches_a_stage_is_dropped_rather_than_looping(self):
        self.tree.write("a.slangp", "#reference b.slangp\n")
        self.tree.write("b.slangp", "#reference a.slangp\n")
        with self.assertRaises(manifest.Unresolved):
            self.resolve("a.slangp")

    def test_a_chain_deeper_than_the_cap_is_unresolved_not_trimmed(self):
        depth = manifest.MAX_DEPTH + 4
        self.tree.write("deep.slangp", "shaders = 1\nshader0 = s0.slang\n")
        for level in range(depth):
            self.tree.write("s%d.slang" % level,
                            '#include "s%d.slang"\n' % (level + 1) + stage(MIT_HEADER))
        self.tree.write("s%d.slang" % depth, stage(MIT_HEADER))
        with self.assertRaises(manifest.Unresolved) as raised:
            self.resolve("deep.slangp")
        self.assertIn("deeper than", str(raised.exception))

    def test_an_escaping_path_an_absolute_path_and_an_encoded_traversal_are_refused(self):
        for target in ("../../outside.slang", "/etc/passwd", "%2e%2e/outside.slang"):
            with self.subTest(target=target):
                self.tree.write("escape.slangp", "shaders = 1\nshader0 = %s\n" % target)
                with self.assertRaises((manifest.Unsafe, manifest.Unresolved)):
                    self.resolve("escape.slangp")

    def test_a_file_that_is_not_valid_utf8_is_read_anyway(self):
        self.tree.write("latin.slangp", "shaders = 1\nshader0 = latin.slang\n")
        self.tree.write("latin.slang",
                        ("// author: caf\xe9\n" + stage(PUBLIC_DOMAIN_HEADER))
                        .encode("iso-8859-1"))
        files, _ = self.resolve("latin.slangp")
        self.assertEqual(len(files), 2)
        text = manifest.Checkout(self.tree.checkout).text("latin.slang")
        self.assertIn("caf\xe9", text)

    def test_a_slangp_with_no_stage_is_a_parameter_fragment_not_a_preset(self):
        self.tree.write("refs/bezel.slangp", 'BEZEL_R = "-0.31"\nBEZEL_G = "-0.31"\n')
        with self.assertRaises(manifest.Unresolved):
            self.resolve("refs/bezel.slangp")


class Buckets(unittest.TestCase):
    """Every .slangp lands in exactly one bucket and the buckets sum to the tree total."""

    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.drop)
        self.tree.write("keep.slangp", "shaders = 1\nshader0 = keep.slang\n")
        self.tree.write("keep.slang", stage(PUBLIC_DOMAIN_HEADER))
        self.tree.write("patched.slangp", "shaders = 1\nshader0 = patched.slang\n")
        self.tree.write("patched.slang", patchable_stage(UNCLAMPED))
        self.tree.patch("prescale.patch", prescale_patch("patched.slang", UNCLAMPED, CLAMPED))

    def classify(self, extra=()):
        code, output = run("classify", self.tree, extra)
        self.assertEqual(code, 0)
        return output

    def buckets(self, extra=()):
        output = self.classify(extra)
        found = {}
        for line in output.splitlines():
            for bucket in manifest.BUCKETS:
                if line.strip().startswith(bucket):
                    found[bucket] = int(line.split()[-1])
        return found

    def test_a_missing_file_drops_the_preset_by_name(self):
        self.tree.write("gone.slangp", "shaders = 1\nshader0 = missing.slang\n")
        self.assertEqual(self.buckets()["unresolved closure"], 1)
        dropped = (self.tree.out / "dropped-presets.csv").read_text(encoding="utf-8")
        self.assertIn("gone.slangp", dropped)
        self.assertIn("missing.slang", dropped)

    def test_an_unclamped_prescale_refuses_the_preset_by_name_and_line(self):
        self.tree.write("black.slangp", "shaders = 1\nshader0 = black.slang\n")
        self.tree.write("black.slang", stage(MIT_HEADER, UNCLAMPED + "\n"))
        self.assertEqual(self.buckets()["unclamped prescale"], 1)
        dropped = (self.tree.out / "dropped-presets.csv").read_text(encoding="utf-8")
        self.assertIn("black.slangp", dropped)
        self.assertIn("black.slang:", dropped)

    def test_the_same_expression_carrying_max_is_accepted(self):
        self.tree.write("clamped.slangp", "shaders = 1\nshader0 = clamped.slang\n")
        self.tree.write("clamped.slang", stage(MIT_HEADER, CLAMPED + "\n"))
        self.assertEqual(self.buckets()["unclamped prescale"], 0)
        self.assertEqual(self.buckets()["offered"], 3)

    def test_an_extension_outside_the_extractor_allowlist_is_refused(self):
        self.tree.write("hlsl.slangp", "shaders = 1\nshader0 = hlsl.slang\n")
        self.tree.write("hlsl.slang", '#include "helper.hlsl"\n' + stage(MIT_HEADER))
        self.tree.write("helper.hlsl", "// helper\n")
        self.assertEqual(self.buckets()["disallowed extension"], 1)

    def test_a_file_over_the_per_entry_cap_is_refused(self):
        self.tree.write("big.slangp", "shaders = 1\nshader0 = big.slang\n")
        self.tree.write("big.slang",
                        stage(MIT_HEADER) + "// " + "x" * manifest.MAX_FILE_BYTES + "\n")
        self.assertEqual(self.buckets()["file over cap"], 1)

    def test_a_closure_over_the_configured_total_is_refused(self):
        self.tree.write("fat.slangp", "shaders = 1\nshader0 = fat.slang\n")
        self.tree.write("fat.slang", stage(MIT_HEADER) + "// " + "x" * 4096 + "\n")
        buckets = self.buckets(["--max-closure-bytes", "3000"])
        self.assertEqual(buckets["closure over cap"], 1)

    def test_the_buckets_sum_to_the_tree_total(self):
        self.tree.write("gone.slangp", "shaders = 1\nshader0 = missing.slang\n")
        self.tree.write("black.slangp", "shaders = 1\nshader0 = black.slang\n")
        self.tree.write("black.slang", stage(MIT_HEADER, UNCLAMPED + "\n"))
        buckets = self.buckets()
        self.assertEqual(sum(buckets.values()), 4)
        self.assertEqual(buckets["offered"], 2)


class Classification(unittest.TestCase):
    """One fixture per class, built from the spellings the audit quotes."""

    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.drop)
        self.checkout = manifest.Checkout(self.tree.checkout)

    def classify(self, relative, text, ceiling=manifest.DEFAULT_CONFIG_BYTES):
        self.tree.write(relative, text)
        self.checkout = manifest.Checkout(self.tree.checkout)
        return self.checkout and manifest.classify_file(
            manifest.Checkout(self.tree.checkout), relative, text, len(text), ceiling)[0]

    def test_each_class_is_reached_by_its_own_evidence(self):
        cases = [
            ("g.slang", stage(GPL_OR_LATER_HEADER), manifest.GPL_OR_LATER),
            ("gv.slang", stage(GPL_VERSION_ONLY_HEADER), manifest.GPL_VERSION_ONLY),
            ("u.slang", stage(GPL_UNVERSIONED_HEADER), manifest.GPL_UNVERSIONED),
            ("m.slang", stage(MIT_HEADER), manifest.PERMISSIVE),
            ("p.slang", stage(PUBLIC_DOMAIN_HEADER), manifest.PUBLIC_DOMAIN),
            ("x.slang", stage(CONFLICTING_HEADER), manifest.UNCLASSIFIED),
            ("c.slangp", "shaders = 1\nshader0 = p.slang\n", manifest.CONFIG_ONLY),
        ]
        for relative, text, expected in cases:
            with self.subTest(relative=relative):
                self.assertEqual(self.classify(relative, text), expected)

    def test_a_header_less_wrapper_is_no_licence_and_never_config_only(self):
        wrapper = ('#define FINEMASK\n#include "impl.inc"\n')
        self.assertEqual(self.classify("zfast_finemask.slang", wrapper),
                         manifest.NO_LICENCE)

    def test_a_slangp_that_includes_source_is_not_config_only(self):
        self.assertEqual(self.classify("odd.slangp",
                                       'shaders = 1\n#include "impl.inc"\n'),
                         manifest.NO_LICENCE)

    def test_a_comment_does_not_disqualify_a_slangp_from_config_only(self):
        self.assertEqual(self.classify("commented.slangp",
                                       "# GTU TV processing\nshaders = 1\n"),
                         manifest.CONFIG_ONLY)

    def test_a_slangp_over_the_stated_ceiling_falls_out_of_config_only(self):
        body = "shaders = 1\n" + "".join("p%d = 0.5\n" % n for n in range(400))
        self.assertEqual(self.classify("huge.slangp", body, ceiling=64),
                         manifest.NO_LICENCE)

    def test_a_header_less_file_under_a_directory_licence_is_its_own_class(self):
        self.tree.write("lic/LICENSE", "GNU GENERAL PUBLIC LICENSE\n")
        self.assertEqual(self.classify("lic/plain.slang", "#version 450\nvoid main(){}\n"),
                         manifest.DIRECTORY_LICENCE)

    def test_the_three_separations_are_not_folded_into_a_neighbour(self):
        for left, right in ((manifest.GPL_VERSION_ONLY, manifest.GPL_OR_LATER),
                            (manifest.GPL_UNVERSIONED, manifest.GPL_OR_LATER),
                            (manifest.DIRECTORY_LICENCE, manifest.NO_LICENCE)):
            self.assertNotEqual(left, right)
            self.assertIn(left, manifest.CLASSES)


class Sampling(unittest.TestCase):
    def test_the_same_tree_draws_the_same_rows_twice(self):
        paths = ["c/%d.slang" % n for n in range(200)]
        first = manifest.sample_rows(manifest.PIN, manifest.GPL_OR_LATER, paths, 30)
        second = manifest.sample_rows(manifest.PIN, manifest.GPL_OR_LATER, paths, 30)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 30)

    def test_the_draw_does_not_depend_on_the_order_the_rows_arrived_in(self):
        paths = ["c/%d.slang" % n for n in range(200)]
        forward = manifest.sample_rows(manifest.PIN, manifest.NO_LICENCE, paths, 10)
        backward = manifest.sample_rows(manifest.PIN, manifest.NO_LICENCE,
                                        list(reversed(paths)), 10)
        self.assertEqual(forward, backward)

    def test_the_draw_is_recomputable_by_a_reader_with_one_line(self):
        paths = ["a.slang", "b.slang", "c.slang"]
        drawn = manifest.sample_rows(manifest.PIN, manifest.PERMISSIVE, paths, 2)
        expected = sorted(paths, key=lambda p: hashlib.sha256(
            (manifest.PIN + manifest.PERMISSIVE + p).encode("utf-8")).hexdigest())[:2]
        self.assertEqual(drawn, expected)

    def test_a_class_smaller_than_the_sample_is_listed_in_full(self):
        paths = ["only.slang"]
        self.assertEqual(manifest.sample_rows(manifest.PIN, manifest.UNCLASSIFIED,
                                              paths, 30), paths)


class Gate(unittest.TestCase):
    """`emit` refuses without a signed rules file at this pin, and writes nothing."""

    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.drop)
        self.tree.write("keep.slangp", "shaders = 1\nshader0 = keep.slang\n")
        self.tree.write("keep.slang", stage(PUBLIC_DOMAIN_HEADER))
        self.tree.write("patched.slangp", "shaders = 1\nshader0 = patched.slang\n")
        self.tree.write("patched.slang", patchable_stage(UNCLAMPED))
        self.tree.patch("prescale.patch", prescale_patch("patched.slang", UNCLAMPED, CLAMPED))

    def assertNothingEmitted(self):
        self.assertFalse((self.tree.out / "manifest.json").exists())
        self.assertFalse((self.tree.out / "zips").exists())

    def test_classify_writes_no_manifest_and_no_zip(self):
        code, _ = run("classify", self.tree)
        self.assertEqual(code, 0)
        self.assertTrue((self.tree.out / "LICENCE-CLASSES-DRAFT.md").exists())
        self.assertTrue((self.tree.out / "licence-evidence.csv").exists())
        self.assertNothingEmitted()

    def test_emit_refuses_with_no_rules_file(self):
        with self.assertRaises(SystemExit):
            run("emit", self.tree)
        self.assertNothingEmitted()

    def test_emit_refuses_an_unsigned_rules_file(self):
        rules = rules_file(self.tree, ADMIT_ALL, signer="TBD")
        with self.assertRaises(SystemExit):
            run("emit", self.tree, ["--rules", str(rules)])
        self.assertNothingEmitted()

    def test_emit_refuses_a_rules_file_recording_another_pin(self):
        rules = rules_file(self.tree, ADMIT_ALL, pin="0" * 40)
        with self.assertRaises(SystemExit):
            run("emit", self.tree, ["--rules", str(rules)])
        self.assertNothingEmitted()

    def test_emit_refuses_a_class_with_a_population_and_no_decision(self):
        partial = {manifest.CONFIG_ONLY: "include"}
        rules = rules_file(self.tree, partial)
        with self.assertRaises(SystemExit):
            run("emit", self.tree, ["--rules", str(rules)])
        self.assertNothingEmitted()

    def test_the_generator_refuses_any_commit_other_than_the_pin(self):
        self.tree.head("1d5a9f03" + "0" * 32)
        with self.assertRaises(SystemExit):
            run("classify", self.tree)


class Emitted(unittest.TestCase):
    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.drop)
        self.tree.write("scanlines/keep.slangp",
                        "shaders = 1\nshader0 = shaders/keep.slang\n")
        self.tree.write("scanlines/shaders/keep.slang",
                        '#include "../../include/masks.h"\n' + stage(PUBLIC_DOMAIN_HEADER))
        self.tree.write("include/masks.h", stage(MIT_HEADER))
        self.tree.write("crt/patched.slangp", "shaders = 1\nshader0 = patched.slang\n")
        self.tree.write("crt/patched.slang", patchable_stage(UNCLAMPED))
        self.tree.write("crt/refused.slangp", "shaders = 1\nshader0 = refused.slang\n")
        self.tree.write("crt/refused.slang", stage(CONFLICTING_HEADER))
        self.tree.patch("prescale.patch",
                        prescale_patch("crt/patched.slang", UNCLAMPED, CLAMPED))
        self.decisions = dict(ADMIT_ALL)
        self.decisions[manifest.UNCLASSIFIED] = "exclude"

    def emit(self, out=None, extra=()):
        rules = rules_file(self.tree, self.decisions)
        arguments = ["--rules", str(rules), "--generated-at", "2026-08-18T00:00:00Z"]
        arguments.extend(extra)
        code, output = run("emit", self.tree, arguments, out=out)
        self.assertEqual(code, 0)
        target = Path(out or self.tree.out)
        return json.loads((target / "manifest.json").read_text(encoding="utf-8")), output

    def test_every_zip_member_is_the_byte_the_manifest_declares_and_the_file_scanned(self):
        document, _ = self.emit()
        self.assertTrue(document["entries"])
        for entry in document["entries"]:
            archive = self.tree.out / entry["zip"]["path"]
            self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(),
                             entry["zip"]["sha256"])
            with zipfile.ZipFile(archive) as opened:
                members = sorted(opened.namelist())
                self.assertEqual(members, sorted(f["path"] for f in entry["files"]))
                for record in entry["files"]:
                    served = opened.read(record["path"])
                    on_disk = (self.tree.checkout / record["path"]).read_bytes()
                    self.assertEqual(served, on_disk)
                    self.assertEqual(hashlib.sha256(served).hexdigest(), record["sha256"])

    def test_a_patched_file_is_offered_fixed_and_says_so(self):
        document, _ = self.emit()
        patched = [e for e in document["entries"] if e["id"] == "crt/patched"]
        self.assertEqual(len(patched), 1)
        record = [f for f in patched[0]["files"] if f["path"].endswith("patched.slang")][0]
        self.assertIsNotNone(record["modified"])
        self.assertEqual(record["modified"]["patch"], "prescale.patch")
        self.assertNotEqual(record["modified"]["upstream_sha256"],
                            record["modified"]["served_sha256"])
        self.assertEqual(record["modified"]["served_sha256"], record["sha256"])
        with zipfile.ZipFile(self.tree.out / patched[0]["zip"]["path"]) as opened:
            self.assertIn("max(", opened.read(record["path"]).decode("utf-8"))

    def test_an_excluded_class_removes_the_preset_and_the_manifest_records_the_rest(self):
        document, output = self.emit()
        identifiers = {entry["id"] for entry in document["entries"]}
        self.assertNotIn("crt/refused", identifiers)
        self.assertIn("scanlines/keep", identifiers)
        self.assertIn("excluded by class", output)

    def test_the_manifest_round_trips_and_every_path_is_safe(self):
        document, _ = self.emit()
        self.assertEqual(document["schema"], 1)
        self.assertEqual(document["pin"], manifest.PIN)
        self.assertTrue(document["rules_sha256"])
        for entry in document["entries"]:
            for field in (entry["preset"], entry["zip"]["path"]):
                self.assertTrue(manifest.is_safe_relative(field), field)
            self.assertEqual(entry["closure_bytes"],
                             sum(f["bytes"] for f in entry["files"]))
            for record in entry["files"]:
                self.assertTrue(manifest.is_safe_relative(record["path"]), record)
                self.assertIn("modified", record)

    def test_regeneration_is_byte_reproducible_when_the_mtimes_move(self):
        first_out = self.tree.base / "one"
        second_out = self.tree.base / "two"
        first, _ = self.emit(out=first_out)
        for path in sorted(self.tree.checkout.rglob("*")):
            if path.is_file():
                os.utime(path, (1000000, 1000000))
        second, _ = self.emit(out=second_out)
        self.assertEqual(first, second)
        for entry in first["entries"]:
            left = (first_out / entry["zip"]["path"]).read_bytes()
            right = (second_out / entry["zip"]["path"]).read_bytes()
            self.assertEqual(left, right)

    def test_the_evidence_table_carries_a_verdict_for_every_row(self):
        self.emit()
        rows = (self.tree.out / "licence-evidence.csv").read_text(encoding="utf-8")
        self.assertIn("verdict", rows.splitlines()[0])
        for line in rows.splitlines()[1:]:
            self.assertTrue(line.rstrip().endswith("include")
                            or line.rstrip().endswith("exclude"), line)


class SharedDetector(unittest.TestCase):
    def test_the_prescale_regex_is_the_bundled_fence_object_not_a_copy(self):
        self.assertIs(manifest.PRESCALE, guard.PRESCALE)
        # re.compile caches, so identity alone cannot see a second literal of the same
        # pattern. The source is what stops the two trees drifting on the spelling.
        source = GENERATOR.read_text(encoding="utf-8")
        self.assertNotIn("PRESCALE = re.compile", source)
        self.assertIn("test_ios_shader_prescale_guard", source)

    def test_the_real_patch_still_turns_the_spelling_this_fence_uses(self):
        body = REAL_PATCH.read_text(encoding="utf-8")
        self.assertIn(UNCLAMPED.strip(), body)
        self.assertIn(CLAMPED.strip(), body)

    def test_the_extension_allowlist_is_read_out_of_the_extractor(self):
        allowed = manifest.extractor_extensions(BRIDGE)
        self.assertIn(".slangp", allowed)
        self.assertIn(".slang", allowed)
        self.assertNotIn(".zip", allowed)
        self.assertNotIn(".hlsl", allowed)


class NoNetwork(unittest.TestCase):
    def test_the_generator_shells_out_only_to_git_apply(self):
        source = GENERATOR.read_text(encoding="utf-8")
        code = "\n".join(line.split("#")[0] for line in source.splitlines())
        for banned in ("urllib", "http.client", "socket."):
            self.assertNotIn(banned, code)
        calls = [line for line in code.splitlines() if "subprocess." in line]
        self.assertTrue(calls)
        for line in calls:
            self.assertIn('["git", "apply"]', line)


if __name__ == "__main__":
    unittest.main()
