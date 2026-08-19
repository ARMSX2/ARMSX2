#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
# SPDX-License-Identifier: GPL-3.0+

"""Oracle scoring harness for the GS renderers.

Scores a renderer arm per-pixel against golden frames produced by the software
renderer, over a corpus of GS dumps. This is the correctness gate for the Tile
renderer ladder: eyeball review is banned by charter, so every claim of "renders
correctly" is a number produced here.

GS dump replay is nondeterministic run-to-run (thread timing changes transfer
interleaving on some dumps), so nothing is compared raw: the golden build runs the
software renderer N times and records which frames are byte-stable across runs, a
score run does the same for the arm under test, and only frames stable in BOTH arms
are compared. The stable fraction is itself a gated metric -- a drop in stability is
triaged as a bug, because it usually is one.

Every differing frame also carries a **perceptual severity profile** from
`gs_perceptual.py` -- a named defect class ("GLOBAL TONE SHIFT", "THIN
GEOMETRY", "LOCALISED DEFECT") beside the raw percentages. That channel is
REPORTING ONLY and never touches `pass`: the level metric remains the gate. It
exists because pixel counts do not rank the way people do -- a full-screen
exposure shift moves 81% of pixels and reads as mild, while a dropped polygon
moves 1.5% and is the first thing an eye lands on. Its consumer is the waiver
workflow, so a recorded quality trade is reviewed with severity in hand.
`--no-perceptual` turns it off.

⚠️ This harness compares PRESENTED frames -- the `*_frame*.png` that gsrunner writes
when given `-dumpdir` alone. It does NOT compare `-dump f` output, which is the
per-draw debugger writing internal render targets. Those targets are sized by each
renderer's own allocation policy and legitimately differ between arms: on one
letterboxed title the same frame dumps 640x415 under sw, 640x448 under tile, and
641x800-varying under classic, none of which is a bug. Reaching for `-dump f` to
investigate a score from here produces three arms that disagree about frame geometry
and an inviting, wrong conclusion. The tell that you are off the rails is that
`score_frame_arrays` reports a shape mismatch as an explicit error, so any run that
produced real percentages was comparing equally-sized frames.

Stated generally, because this harness has now paid for it twice: prove you are
comparing the artifact you think you are, the same way you prove each arm really is
the renderer it claims to be.

Subcommands:
  golden    build/refresh golden frames + stability manifests for a corpus
  score     run one dump under an arm and score it against its golden
  corpus    score a whole corpus, apply reviewed waivers, emit a scorecard JSON
  selftest  prove the scorer catches an injected 1-level/1% shift (no runner needed)

Arms (what gets executed, verified by effect against the renderer identity line in
the emulog rather than trusted from the command line):
  sw       -renderer sw                        expect no variant identity line
  classic  -renderer vulkan                    expect "GS: Classic renderer active"
  tile     -renderer vulkan HWRendererVariant=2  expect "GS: Tile renderer active"
  tilegpu  -renderer vulkan HWRendererVariant=3  expect "GS: TileGpu renderer active"

The classic arm deliberately leaves HWRendererVariant at its Auto default: that is
the configuration real users run today, and Auto currently resolves Classic
everywhere (and prints the identity line, which explicit Classic does not).

Golden layout (lives beside the dumps, outside any repo -- goldens are ~1 GB):
  <golden-root>/corpus_summary.json
  <golden-root>/<gsname>/manifest.json
  <golden-root>/<gsname>/frames/<title>_frameNNNNN.png

Typical use:
  ./gs_oracle.py golden  --runner build/bin/pcsx2-gsrunner --corpus corpus.txt --golden-root ~/gs-oracle/golden
  ./gs_oracle.py score   --runner build/bin/pcsx2-gsrunner --golden-root ~/gs-oracle/golden --arm tile --dump <dump>
  ./gs_oracle.py corpus  --runner build/bin/pcsx2-gsrunner --golden-root ~/gs-oracle/golden --corpus corpus.txt \
                         --arm classic --out classic_baseline.json
  ./gs_oracle.py selftest --golden-root ~/gs-oracle/golden
"""

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SCRIPT_DIR = os.path.abspath(os.path.dirname(__file__))

# Thresholds (in 8-bit levels) the scorer always reports. Gates pick one of these.
THRESHOLDS = [0, 1, 2, 4, 8]

DUMP_EXTENSIONS = (".gs", ".gs.xz", ".gs.zst")

# TileGpu must precede Tile in the alternation: the TileGpu identity line starts
# with the same four letters, and "Tile" alone would match it and misreport the arm.
IDENTITY_RE = re.compile(r"GS: (TileGpu|Tile|Classic) renderer active \(([^)]*)\)")

ARMS = {
    "sw": {"args": ["-renderer", "sw"], "identity": None},
    "classic": {"args": ["-renderer", "vulkan"], "identity": "Classic"},
    "tile": {
        "args": ["-renderer", "vulkan", "-set", "EmuCore/GS/HWRendererVariant=2"],
        "identity": "Tile",
    },
    "tilegpu": {
        "args": ["-renderer", "vulkan", "-set", "EmuCore/GS/HWRendererVariant=3"],
        "identity": "TileGpu",
    },
}


def log(msg):
    print(msg, flush=True)


def gs_name(path):
    """Dump basename with the .gs[.xz|.zst] extension stripped; the corpus key."""
    base = os.path.basename(path)
    lower = base.lower()
    for ext in DUMP_EXTENSIONS:
        if lower.endswith(ext):
            return base[: -len(ext)]
    return None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_describe():
    try:
        return subprocess.run(
            ["git", "-C", SCRIPT_DIR, "describe", "--always", "--dirty"],
            capture_output=True, text=True, timeout=10,
        ).stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def load_corpus(path):
    dumps = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if gs_name(line) is None:
                raise SystemExit(f"corpus entry is not a GS dump: {line}")
            if not os.path.isfile(line):
                raise SystemExit(f"corpus dump does not exist: {line}")
            dumps.append(line)
    if not dumps:
        raise SystemExit(f"corpus file {path} lists no dumps")
    return dumps


# ---------------------------------------------------------------------------
# Running an arm
# ---------------------------------------------------------------------------

def run_gsrunner_once(runner, dump, run_dir, arm_args, loop, upscale, extra_args,
                      timeout):
    """One gsrunner invocation. Returns a per-run record; never raises on runner
    failure -- failures are data."""
    os.makedirs(run_dir, exist_ok=True)
    logfile = os.path.join(run_dir, "emulog.txt")
    statsfile = os.path.join(run_dir, "stats.json")

    args = [runner]
    args += arm_args
    args += ["-dumpdir", run_dir, "-logfile", logfile, "-loop", str(loop)]
    args += ["-stats-json", statsfile]
    if upscale is not None:
        args += ["-upscale", str(upscale)]
    args += ["-surfaceless"]
    args += list(extra_args)
    args += ["--", dump]

    env = os.environ.copy()
    env["PCSX2_NOCONSOLE"] = "1"

    started = time.monotonic()
    try:
        proc = subprocess.run(args, env=env, stdin=subprocess.DEVNULL,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=timeout)
        returncode = proc.returncode
        timed_out = False
    except subprocess.TimeoutExpired:
        returncode = None
        timed_out = True
    wall_s = time.monotonic() - started

    frames = {}
    for path in glob.glob(os.path.join(glob.escape(run_dir), "*_frame*.png")):
        m = re.match(r".*_frame(\d+)\.png$", os.path.basename(path))
        if m:
            frames[int(m.group(1))] = path

    hashes = {idx: sha256_file(path) for idx, path in sorted(frames.items())}

    identity = None
    try:
        with open(logfile, "r", errors="ignore") as f:
            for line in f:
                m = IDENTITY_RE.search(line)
                if m:
                    identity = {"renderer": m.group(1), "detail": m.group(2)}
                    break
    except OSError:
        pass

    device = None
    try:
        with open(statsfile, "r", encoding="utf-8") as f:
            stats = json.load(f)
        run = stats.get("run", {})
        device = {k: run.get(k) for k in
                  ("device_name", "driver_info", "frames", "pipeline_switches")}
    except (OSError, ValueError):
        pass

    return {
        "dir": run_dir,
        "returncode": returncode,
        "timed_out": timed_out,
        "wall_s": round(wall_s, 1),
        "frames": frames,
        "hashes": hashes,
        "identity": identity,
        "device": device,
        "cmdline": args,
    }


def verify_identity(arm, runs):
    """Verify-by-effect: the emulog identity line must match the requested arm in
    every run. Returns (ok, message)."""
    expect = ARMS[arm]["identity"]
    for i, run in enumerate(runs):
        got = run["identity"]["renderer"] if run["identity"] else None
        if got != expect:
            return False, (f"run {i}: expected identity {expect!r}, emulog says "
                           f"{got!r} -- the arm did not run what was asked")
    return True, "ok"


def run_arm(runner, dump, arm, work_dir, runs, loop, upscale, extra_args, timeout):
    """Run an arm N times; collate stability. Returns (runs list, stability dict)."""
    run_records = []
    for i in range(runs):
        run_dir = os.path.join(work_dir, f"run{i}")
        rec = run_gsrunner_once(runner, dump, run_dir, ARMS[arm]["args"], loop,
                                upscale, extra_args, timeout)
        run_records.append(rec)

    # A frame index is stable when present in every run with one identical hash.
    all_indices = sorted(set().union(*(r["hashes"].keys() for r in run_records)))
    stable = {}
    for idx in all_indices:
        hashes = [r["hashes"].get(idx) for r in run_records]
        stable[idx] = (None not in hashes) and len(set(hashes)) == 1

    stability = {
        "indices": all_indices,
        "stable": stable,
        "stable_fraction": (sum(stable.values()) / len(all_indices)) if all_indices else 0.0,
    }
    return run_records, stability


# ---------------------------------------------------------------------------
# Pixel scoring
# ---------------------------------------------------------------------------

def load_png_array(path):
    from PIL import Image  # lazy: hash-only paths must not require it
    import numpy as np

    img = Image.open(path)
    if img.mode not in ("RGB", "RGBA"):
        img = img.convert("RGBA")
    return np.asarray(img)


def score_frame_arrays(golden, test):
    """Per-pixel metrics between two u8 arrays. A pixel is wrong at threshold T when
    any channel differs by more than T levels."""
    import numpy as np

    if golden.shape != test.shape:
        # Normalize an RGB-vs-RGBA mismatch; anything else is a real failure.
        if golden.shape[:2] == test.shape[:2] and 3 in (golden.shape[2], test.shape[2]):
            if golden.shape[2] == 3:
                golden = np.dstack([golden, np.full(golden.shape[:2], 255, np.uint8)])
            if test.shape[2] == 3:
                test = np.dstack([test, np.full(test.shape[:2], 255, np.uint8)])
        else:
            return {"error": f"shape mismatch: golden {golden.shape} vs test {test.shape}"}

    diff = np.abs(golden.astype(np.int16) - test.astype(np.int16))
    pixel_max = diff.max(axis=2)
    total = pixel_max.size

    pct_gt = {str(t): round(float((pixel_max > t).sum()) * 100.0 / total, 4)
              for t in THRESHOLDS}
    per_channel = [round(float((diff[:, :, c] > 0).sum()) * 100.0 / total, 4)
                   for c in range(diff.shape[2])]

    return {
        "pct_gt": pct_gt,
        "per_channel_pct_gt0": per_channel,
        "max_diff": int(diff.max()),
        "mean_diff": round(float(diff.mean()), 5),
    }


def perceptual_profile(golden_path, test_path, ppd):
    """Perceptual severity for one differing frame, or None if unavailable.

    ⚠️ REPORTING ONLY. This never touches `pass` -- the gate stays the level
    metric. Its consumer is the waiver workflow: a scored quality trade is
    reviewed with a severity profile in hand, which is the difference between
    "81.6% of pixels moved" and "one global gain of 1.06, no structural
    residue". The eyeball ban is on eyes as the *gate*, not on informed
    judgement over an explicitly recorded trade.

    Never let this break a scoring run: it is commentary on a number that has
    already been computed.
    """
    try:
        if SCRIPT_DIR not in sys.path:
            sys.path.insert(0, SCRIPT_DIR)  # works when imported, not just run
        import gs_perceptual
    except ImportError:
        return None
    try:
        block = gs_perceptual.compare_files(golden_path, test_path, ppd=ppd)
    except Exception as e:  # noqa: BLE001 - commentary must not fail the gate
        return {"error": f"{type(e).__name__}: {e}"}
    return {
        "severity": block["severity"],
        "verdict": block["verdict"],
        "detail": block["detail"],
        "flip_mean": block.get("flip_mean"),
        "flip_weighted_median": block.get("flip_weighted_median"),
        "de_p99_9": block.get("de_p99_9"),
        "de_max": block.get("de_max"),
        "above_jnd_pct": block.get("above_jnd_pct"),
        "erosion_survival": block.get("erosion_survival"),
        "largest_blob_px": block.get("largest_blob_px"),
        "largest_blob_bbox": block.get("largest_blob_bbox"),
        "affine_explained": block.get("affine_explained"),
        "ppd": block.get("ppd"),
    }


def find_waiver(waivers, gsname, frame_idx):
    for w in waivers:
        if w.get("dump") != gsname:
            continue
        frames = w.get("frames", "all")
        if frames == "all" or (isinstance(frames, list) and frame_idx in frames):
            return w
    return None


def score_against_golden(manifest, golden_frames_dir, test_runs, test_stability,
                         gate_threshold, gate_pct, min_stable_fraction, hash_only,
                         waivers, perceptual=True, ppd=None):
    """Compare a test arm's stable frames against a golden manifest. Returns the
    per-dump scorecard dict (schema is the harness's public contract; bump
    'schema' on any incompatible change)."""
    gsname = manifest["gsname"]
    golden_hashes = {int(k): v for k, v in manifest["frame_hashes_run0"].items()}
    golden_stable = {int(k): v for k, v in manifest["stable"].items()}
    golden_files = {int(k): v for k, v in manifest["frame_files"].items()}

    rep = test_runs[0]  # stability guarantees any run is representative on stable frames

    frames_out = []
    failed = waived = identical = diffed = missing = skipped = 0

    for idx in sorted(golden_stable.keys()):
        if not golden_stable[idx]:
            frames_out.append({"frame": idx, "status": "skipped_unstable_golden"})
            skipped += 1
            continue
        if not test_stability["stable"].get(idx, False):
            if idx not in test_stability["stable"]:
                # A golden-stable frame the test arm never produced is a failure,
                # not an instability: the run ended early or dropped a present.
                frames_out.append({"frame": idx, "status": "missing", "pass": False})
                missing += 1
                failed += 1
            else:
                frames_out.append({"frame": idx, "status": "skipped_unstable_test"})
                skipped += 1
            continue

        entry = {"frame": idx}
        if rep["hashes"][idx] == golden_hashes[idx]:
            entry["status"] = "identical"
            entry["pass"] = True
            identical += 1
            frames_out.append(entry)
            continue

        if hash_only:
            entry["status"] = "diff"
            entry["pass"] = False
            diffed += 1
            failed += 1
            frames_out.append(entry)
            continue

        golden_path = os.path.join(golden_frames_dir, golden_files[idx])
        metrics = score_frame_arrays(load_png_array(golden_path),
                                     load_png_array(rep["frames"][idx]))
        entry["status"] = "diff"
        entry.update(metrics)
        if "error" in metrics:
            entry["pass"] = False
        else:
            entry["pass"] = metrics["pct_gt"][str(gate_threshold)] <= gate_pct
        # Attached after the verdict is already decided, so it cannot influence it.
        if perceptual and "error" not in metrics:
            p = perceptual_profile(golden_path, rep["frames"][idx], ppd)
            if p is not None:
                entry["perceptual"] = p
        diffed += 1

        if not entry["pass"]:
            w = find_waiver(waivers, gsname, idx)
            if w is not None and "error" not in metrics:
                w_threshold = str(w.get("threshold", gate_threshold))
                w_max_pct = float(w.get("max_pct", 0.0))
                if metrics["pct_gt"].get(w_threshold, 100.0) <= w_max_pct:
                    entry["pass"] = True
                    entry["waived"] = True
                    entry["waiver_reason"] = w.get("reason", "")
                    waived += 1
        if not entry["pass"]:
            failed += 1
        frames_out.append(entry)

    total_golden = len(golden_stable)
    compared = identical + diffed
    compared_fraction = compared / total_golden if total_golden else 0.0

    stability_ok = (min(manifest["stable_fraction"],
                        test_stability["stable_fraction"]) >= min_stable_fraction)

    worst = None
    for f in frames_out:
        if f.get("status") == "diff" and "pct_gt" in f:
            if worst is None or f["pct_gt"]["0"] > worst["pct_gt"]["0"]:
                worst = f

    # The perceptually worst frame is often NOT the one with the most wrong
    # pixels -- that divergence is the entire point of carrying both.
    severity_rank = ["invisible", "subtle", "noticeable", "obvious", "gross"]
    worst_perceptual = None
    for f in frames_out:
        p = f.get("perceptual")
        if not p or "severity" not in p:
            continue
        if (worst_perceptual is None
                or severity_rank.index(p["severity"])
                > severity_rank.index(worst_perceptual["severity"])):
            worst_perceptual = {"frame": f["frame"], **p}

    return {
        "schema": 2,
        "gsname": gsname,
        "dump": manifest["dump"],
        "pass": failed == 0 and stability_ok,
        "gate": {"threshold": gate_threshold, "max_pct": gate_pct,
                 "min_stable_fraction": min_stable_fraction, "hash_only": hash_only},
        "frames": frames_out,
        "summary": {
            "golden_frames": total_golden,
            "compared": compared,
            "compared_fraction": round(compared_fraction, 4),
            "identical": identical,
            "diff": diffed,
            "missing": missing,
            "failed": failed,
            "waived": waived,
            "skipped_unstable": skipped,
            "stable_fraction_golden": round(manifest["stable_fraction"], 4),
            "stable_fraction_test": round(test_stability["stable_fraction"], 4),
            "stability_ok": stability_ok,
            "worst": ({"frame": worst["frame"], "pct_gt": worst["pct_gt"],
                       "max_diff": worst.get("max_diff")} if worst else None),
            "worst_perceptual": worst_perceptual,
        },
    }


# ---------------------------------------------------------------------------
# golden
# ---------------------------------------------------------------------------

def build_golden_for_dump(args, dump):
    gsname = gs_name(dump)
    out_dir = os.path.join(args.golden_root, gsname)
    manifest_path = os.path.join(out_dir, "manifest.json")

    dump_hash = sha256_file(dump)
    if os.path.isfile(manifest_path) and not args.force:
        with open(manifest_path, "r", encoding="utf-8") as f:
            existing = json.load(f)
        if existing.get("dump_sha256") == dump_hash and existing.get("loop") == args.loop:
            log(f"  KEEP  {gsname} (golden up to date)")
            return existing

    with tempfile.TemporaryDirectory(prefix="gs_oracle_golden_") as work_dir:
        runs, stability = run_arm(args.runner, dump, "sw", work_dir, args.runs,
                                  args.loop, args.upscale, [], args.timeout)

        for i, r in enumerate(runs):
            if r["timed_out"] or r["returncode"] != 0:
                raise SystemExit(f"golden run {i} of {gsname} failed "
                                 f"(rc={r['returncode']}, timeout={r['timed_out']}); "
                                 f"kept nothing. cmd: {' '.join(r['cmdline'])}")
        ok, msg = verify_identity("sw", runs)
        if not ok:
            raise SystemExit(f"golden identity check failed for {gsname}: {msg}")
        if not stability["indices"]:
            raise SystemExit(f"golden run of {gsname} produced no frames")

        # Frames come from run 0; the manifest records every run's hashes so a
        # nightly can re-check hash-only without ever decoding pixels.
        if os.path.isdir(out_dir):
            shutil.rmtree(out_dir)
        frames_dir = os.path.join(out_dir, "frames")
        os.makedirs(frames_dir)
        frame_files = {}
        for idx, path in runs[0]["frames"].items():
            name = os.path.basename(path)
            shutil.copyfile(path, os.path.join(frames_dir, name))
            frame_files[str(idx)] = name

        manifest = {
            "schema": 1,
            "gsname": gsname,
            "dump": dump,
            "dump_sha256": dump_hash,
            "arm": "sw",
            "runs": args.runs,
            "loop": args.loop,
            "upscale": args.upscale,
            "frame_count": len(stability["indices"]),
            "frame_files": frame_files,
            "frame_hashes_run0": {str(i): h for i, h in runs[0]["hashes"].items()},
            "frame_hashes_all_runs": [
                {str(i): h for i, h in r["hashes"].items()} for r in runs
            ],
            "stable": {str(i): stability["stable"][i] for i in stability["indices"]},
            "stable_fraction": stability["stable_fraction"],
            "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "git": git_describe(),
        }
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=1)

    sf = manifest["stable_fraction"]
    flag = "" if sf >= args.min_stable_fraction else "  ** LOW STABILITY **"
    log(f"  BUILT {gsname}: {manifest['frame_count']} frames, "
        f"stable {sf * 100:.0f}%{flag}")
    return manifest


def cmd_golden(args):
    dumps = load_corpus(args.corpus)
    os.makedirs(args.golden_root, exist_ok=True)
    log(f"Building goldens for {len(dumps)} dumps (sw arm, {args.runs} runs each, "
        f"loop {args.loop})")

    summary = {"created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "git": git_describe(), "runs": args.runs, "loop": args.loop,
               "dumps": {}}
    for dump in dumps:
        manifest = build_golden_for_dump(args, dump)
        summary["dumps"][manifest["gsname"]] = {
            "frames": manifest["frame_count"],
            "stable_fraction": manifest["stable_fraction"],
        }

    fractions = [d["stable_fraction"] for d in summary["dumps"].values()]
    summary["aggregate"] = {
        "mean_stable_fraction": round(sum(fractions) / len(fractions), 4),
        "min_stable_fraction": round(min(fractions), 4),
    }
    with open(os.path.join(args.golden_root, "corpus_summary.json"), "w",
              encoding="utf-8") as f:
        json.dump(summary, f, indent=1)

    log(f"Stable-frame fraction: mean {summary['aggregate']['mean_stable_fraction'] * 100:.1f}%, "
        f"min {summary['aggregate']['min_stable_fraction'] * 100:.1f}%")
    return 0


# ---------------------------------------------------------------------------
# score / corpus
# ---------------------------------------------------------------------------

def load_waivers(path, arm):
    # Waivers document accepted quality trades for the Tile renderer only; a waiver
    # must never quietly excuse a Classic or SW regression.
    if not path or arm != "tile":
        return []
    if not os.path.isfile(path):
        return []
    import yaml
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    return data.get("waivers", []) or []


def score_one_dump(args, dump, waivers):
    gsname = gs_name(dump)
    golden_dir = os.path.join(args.golden_root, gsname)
    manifest_path = os.path.join(golden_dir, "manifest.json")
    if not os.path.isfile(manifest_path):
        raise SystemExit(f"no golden for {gsname}; run `golden` first")
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    if manifest.get("dump_sha256") != sha256_file(dump):
        raise SystemExit(f"dump {gsname} changed since its golden was built; "
                         f"re-run `golden`")

    work_dir = os.path.join(args.workdir, gsname) if args.workdir else \
        tempfile.mkdtemp(prefix=f"gs_oracle_{args.arm}_")
    extra = []
    for kv in (args.set or []):
        extra += ["-set", kv]
    runs, stability = run_arm(args.runner, dump, args.arm, work_dir, args.runs,
                              manifest["loop"], manifest["upscale"], extra,
                              args.timeout)

    hard_fail = None
    for i, r in enumerate(runs):
        if r["timed_out"] or r["returncode"] != 0:
            hard_fail = f"run {i} failed (rc={r['returncode']}, timeout={r['timed_out']})"
            break
    if hard_fail is None:
        ok, msg = verify_identity(args.arm, runs)
        if not ok:
            hard_fail = msg

    if hard_fail is not None:
        card = {"schema": 2, "gsname": gsname, "dump": dump, "pass": False,
                "error": hard_fail, "frames": [], "summary": None}
    else:
        card = score_against_golden(manifest, os.path.join(golden_dir, "frames"),
                                    runs, stability, args.gate_threshold,
                                    args.gate_pct, args.min_stable_fraction,
                                    args.hash_only, waivers,
                                    perceptual=not args.no_perceptual,
                                    ppd=args.ppd)

    card["arm"] = args.arm
    card["arm_settings"] = list(args.set or [])
    card["runs"] = args.runs
    card["identity"] = runs[0]["identity"]
    card["device"] = runs[0]["device"]

    if card["pass"] and not args.keep_runs:
        shutil.rmtree(work_dir, ignore_errors=True)
    else:
        card["workdir"] = work_dir

    return card


def print_card_line(card):
    status = "PASS" if card["pass"] else "FAIL"
    name = card["gsname"][:48]
    if card.get("error"):
        log(f"  {status}  {name:<48} {card['error']}")
        return
    s = card["summary"]
    detail = (f"{s['identical']}/{s['compared']} identical"
              f", sf(g)={s['stable_fraction_golden']:.2f}"
              f", sf(t)={s['stable_fraction_test']:.2f}")
    if s["waived"]:
        detail += f", {s['waived']} waived"
    if s["worst"]:
        w = s["worst"]
        detail += (f", worst f{w['frame']}: {w['pct_gt']['0']}%>0lv "
                   f"{w['pct_gt']['2']}%>2lv max{w['max_diff']}")
    if s["missing"]:
        detail += f", {s['missing']} missing"
    log(f"  {status}  {name:<48} {detail}")
    wp = s.get("worst_perceptual")
    if wp:
        log(f"        perceptual f{wp['frame']}: {wp['severity'].upper()} "
            f"{wp['verdict']} -- {wp['detail']}")


def cmd_score(args):
    waivers = load_waivers(args.waivers, args.arm)
    card = score_one_dump(args, args.dump, waivers)
    print_card_line(card)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(card, f, indent=1)
        log(f"scorecard: {args.out}")
    if not card["pass"] and card.get("workdir"):
        log(f"kept failing runs in {card['workdir']}")
    return 0 if card["pass"] else 1


def compare_to_baseline(cards, baseline_path, regress_eps):
    """Regression check against a pinned scorecard. A dump regresses when its worst
    frame's wrong-pixel percentage (at any reported threshold) worsens by more than
    regress_eps percentage points."""
    with open(baseline_path, "r", encoding="utf-8") as f:
        baseline = json.load(f)
    base_cards = {c["gsname"]: c for c in baseline.get("cards", [])}

    regressions = []
    for card in cards:
        base = base_cards.get(card["gsname"])
        if base is None:
            continue  # new dump; informational only
        if base.get("pass") and not card["pass"]:
            regressions.append(f"{card['gsname']}: was PASS, now FAIL")
            continue
        b_worst = (base.get("summary") or {}).get("worst")
        c_worst = (card.get("summary") or {}).get("worst")
        if c_worst is None:
            continue  # now byte-clean; strictly better
        for t in map(str, THRESHOLDS):
            base_pct = b_worst["pct_gt"].get(t, 0.0) if b_worst else 0.0
            if c_worst["pct_gt"].get(t, 0.0) > base_pct + regress_eps:
                regressions.append(
                    f"{card['gsname']}: worst >{t}lv {base_pct}% -> "
                    f"{c_worst['pct_gt'][t]}%")
                break
    missing = [name for name in base_cards if name not in {c["gsname"] for c in cards}]
    for name in missing:
        regressions.append(f"{name}: present in baseline, absent from this run")
    return regressions


def cmd_corpus(args):
    dumps = load_corpus(args.corpus)
    waivers = load_waivers(args.waivers, args.arm)
    log(f"Scoring {len(dumps)} dumps under arm '{args.arm}' "
        f"(gate: <= {args.gate_pct}% of pixels wrong by > {args.gate_threshold} levels"
        f"{', hash-only' if args.hash_only else ''})")

    cards = [score_one_dump(args, dump, waivers) for dump in dumps]
    for card in cards:
        print_card_line(card)

    passed = sum(1 for c in cards if c["pass"])
    waived_frames = sum((c.get("summary") or {}).get("waived", 0) for c in cards)
    result = {
        "schema": 1,
        "arm": args.arm,
        "arm_settings": list(args.set or []),
        "gate": {"threshold": args.gate_threshold, "max_pct": args.gate_pct,
                 "min_stable_fraction": args.min_stable_fraction,
                 "hash_only": args.hash_only},
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "git": git_describe(),
        "pass": passed == len(cards),
        "dumps_passed": passed,
        "dumps_total": len(cards),
        "waived_frames": waived_frames,
        "cards": cards,
    }

    regressions = []
    if args.baseline:
        regressions = compare_to_baseline(cards, args.baseline, args.regress_eps)
        result["baseline"] = args.baseline
        result["regressions"] = regressions
        for r in regressions:
            log(f"  REGRESSION  {r}")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=1)
        log(f"scorecard: {args.out}")

    if args.regressions_only:
        # Drift-watch mode (e.g. the Classic arm against its pinned baseline): the
        # absolute gate verdict is known-red and not the question; hard run errors
        # and worsening against the baseline are.
        if not args.baseline:
            raise SystemExit("--regressions-only requires --baseline")
        ok = not regressions and not any(c.get("error") for c in cards)
        log(f"== drift-watch: {len(regressions)} regressions vs baseline"
            + (f", {waived_frames} waived frames" if waived_frames else "")
            + f" => {'PASS' if ok else 'FAIL'} ==")
    else:
        ok = result["pass"] and not regressions
        log(f"== {passed}/{len(cards)} dumps pass"
            + (f", {waived_frames} waived frames" if waived_frames else "")
            + (f", {len(regressions)} regressions" if regressions else "")
            + f" => {'PASS' if ok else 'FAIL'} ==")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------

def cmd_selftest(args):
    """Prove the scorer detects an injected 1-level shift on 1% of pixels, and
    classifies its magnitude correctly. Runs on golden pixels alone."""
    import numpy as np

    manifests = sorted(glob.glob(os.path.join(glob.escape(args.golden_root), "*",
                                              "manifest.json")))
    golden_path = None
    for mp in manifests:
        with open(mp, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        for idx_str, is_stable in sorted(manifest["stable"].items(), key=lambda kv: int(kv[0])):
            if is_stable:
                golden_path = os.path.join(os.path.dirname(mp), "frames",
                                           manifest["frame_files"][idx_str])
                break
        if golden_path:
            break
    if golden_path is None:
        raise SystemExit("selftest needs at least one golden with a stable frame; "
                         "run `golden` first")

    golden = load_png_array(golden_path)
    h, w = golden.shape[:2]
    total = h * w
    n_perturb = max(1, total // 100)  # 1% of pixels
    rng = np.random.default_rng(0xA125)  # deterministic: a selftest must not flake
    flat = rng.choice(total, size=n_perturb, replace=False)
    ys, xs = np.unravel_index(flat, (h, w))

    failures = []

    def check(name, cond, detail):
        if cond:
            log(f"  ok    {name}")
        else:
            failures.append(name)
            log(f"  FAIL  {name}: {detail}")

    # 1. Identical copy scores clean.
    clean = score_frame_arrays(golden, golden.copy())
    check("identical copy scores 0% at every threshold",
          all(v == 0.0 for v in clean["pct_gt"].values()),
          f"got {clean['pct_gt']}")

    def perturb(levels):
        t = golden.copy()
        chan = t[ys, xs, 1].astype(np.int16)  # green; direction avoids clipping
        chan = np.where(chan + levels <= 255, chan + levels, chan - levels)
        t[ys, xs, 1] = chan.astype(np.uint8)
        return t

    # 2. The M1 acceptance gate: a 1-level shift on 1% of pixels is caught.
    m1 = score_frame_arrays(golden, perturb(1))
    pct0, pct1 = m1["pct_gt"]["0"], m1["pct_gt"]["1"]
    check("1-level/1% shift detected at threshold 0",
          0.9 <= pct0 <= 1.1, f"pct_gt0={pct0}")
    check("1-level shift classified as <=1 level", pct1 == 0.0, f"pct_gt1={pct1}")
    check("1-level/1% shift FAILS the default gate (0 levels, 0%)",
          pct0 > 0.0, f"pct_gt0={pct0}")

    # 3. Magnitude classification: a 3-level shift crosses the 2-level threshold.
    m3 = score_frame_arrays(golden, perturb(3))
    check("3-level/1% shift crosses the 2-level threshold",
          0.9 <= m3["pct_gt"]["2"] <= 1.1, f"pct_gt2={m3['pct_gt']['2']}")
    check("3-level shift stays under the 4-level threshold",
          m3["pct_gt"]["4"] == 0.0, f"pct_gt4={m3['pct_gt']['4']}")

    log(f"selftest against {os.path.basename(golden_path)} "
        f"({w}x{h}, {n_perturb} pixels perturbed): "
        + ("PASS" if not failures else f"FAIL ({', '.join(failures)})"))
    return 0 if not failures else 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def add_common_run_args(p):
    p.add_argument("--runner", required=True, help="path to pcsx2-gsrunner")
    p.add_argument("--timeout", type=int, default=1800,
                   help="per-run timeout in seconds (default 1800)")


def add_score_args(p):
    p.add_argument("--arm", choices=sorted(ARMS.keys()), default="tile")
    p.add_argument("--set", action="append", metavar="KEY=VALUE",
                   help="extra `-set KEY=VALUE` for the arm under test (repeatable): "
                        "the ablation and unlock levers, e.g. "
                        "EmuCore/GS/TilePerspectiveNative=true. Recorded in the "
                        "scorecard so a lifted card cannot pass for a shipped one")
    p.add_argument("--runs", type=int, default=2,
                   help="runs per dump for test-side stability (default 2)")
    p.add_argument("--gate-threshold", type=int, default=0, choices=THRESHOLDS,
                   help="a pixel is wrong when a channel differs by more than this "
                        "many levels (default 0 = byte-exact)")
    p.add_argument("--gate-pct", type=float, default=0.0,
                   help="max %% of wrong pixels a frame may have (default 0.0)")
    p.add_argument("--min-stable-fraction", type=float, default=0.6,
                   help="fail a dump whose stable-frame fraction (either arm) drops "
                        "below this (default 0.6)")
    p.add_argument("--hash-only", action="store_true",
                   help="byte-hash comparison only; any diff fails, no pixel metrics "
                        "(the cheap nightly mode)")
    p.add_argument("--waivers", default=os.path.join(SCRIPT_DIR, "tile_waivers.yaml"),
                   help="reviewed quality-trade waivers (applied to the tile arm only)")
    p.add_argument("--no-perceptual", action="store_true",
                   help="skip the perceptual severity profile on differing "
                        "frames (it is reporting-only and never gates, but it "
                        "costs ~0.2 s per differing frame)")
    p.add_argument("--ppd", type=float, default=None,
                   help="pixels-per-degree viewing model for the perceptual "
                        "profile; default is the handheld geometry. Perceptual "
                        "scores are only comparable at a fixed ppd.")
    p.add_argument("--workdir", default=None,
                   help="keep run output under this directory (default: temp, "
                        "deleted on pass)")
    p.add_argument("--keep-runs", action="store_true",
                   help="keep run output even on pass")
    p.add_argument("--out", default=None, help="write scorecard JSON here")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("golden", help="build golden frames + stability manifests")
    add_common_run_args(p)
    p.add_argument("--corpus", required=True, help="text file of dump paths")
    p.add_argument("--golden-root", required=True)
    p.add_argument("--runs", type=int, default=3,
                   help="sw runs per dump for the stability manifest (default 3)")
    p.add_argument("--loop", type=int, default=2,
                   help="gsrunner -loop; baked into the manifest (default 2)")
    p.add_argument("--upscale", type=float, default=None,
                   help="gsrunner -upscale; default native")
    p.add_argument("--min-stable-fraction", type=float, default=0.6)
    p.add_argument("--force", action="store_true",
                   help="rebuild goldens even when up to date")
    p.set_defaults(func=cmd_golden)

    p = sub.add_parser("score", help="score one dump against its golden")
    add_common_run_args(p)
    p.add_argument("--golden-root", required=True)
    p.add_argument("--dump", required=True)
    add_score_args(p)
    p.set_defaults(func=cmd_score)

    p = sub.add_parser("corpus", help="score a corpus; apply waivers; gate")
    add_common_run_args(p)
    p.add_argument("--golden-root", required=True)
    p.add_argument("--corpus", required=True)
    add_score_args(p)
    p.add_argument("--baseline", default=None,
                   help="pinned scorecard JSON to detect regressions against")
    p.add_argument("--regress-eps", type=float, default=0.05,
                   help="percentage points of worsening tolerated vs the baseline "
                        "(default 0.05)")
    p.add_argument("--regressions-only", action="store_true",
                   help="exit code reflects baseline regressions and run errors "
                        "only, not the absolute gate (drift-watch mode)")
    p.set_defaults(func=cmd_corpus)

    p = sub.add_parser("selftest",
                       help="prove the scorer catches an injected 1-level/1%% shift")
    p.add_argument("--golden-root", required=True)
    p.set_defaults(func=cmd_selftest)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
