#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
# SPDX-License-Identifier: GPL-3.0+

"""Perceptual severity profile for a pair of renderer screenshots.

`gs_oracle.py` answers "how many pixels moved". This answers "would anyone
notice, and what kind of wrong is it".

The two questions come apart, and they fail in two different ways. Keeping them
apart matters, because only one of them is a general claim:

**Counting wrong pixels reverses the ordering.** Shadow of the Colossus under
the Classic renderer scores 81.58% of pixels wrong by more than two levels and
reads as a mild bloom shift, while a single wrong 64x64 patch -- a dropped
polygon, a wrong texture -- moves 1.5% and is the first thing an eye lands on.
Ranked by pixel count, the catastrophe comes last.

**Averaging the error compresses rather than reverses**, which is just as fatal
for a threshold and is the robust claim. A dE-100 two-pixel seam and an
*invisible* dither haze land **1.04x apart** under mean absolute difference --
indistinguishable -- where the error-weighted median separates them **83x**.
That is measured on a real golden frame and `--selftest` pins it.

(An average can also genuinely invert: mean FLIP ranks a missing polygon at
0.0063 against benign +-1 LSB noise at 0.0138 on a synthetic scene. But that did
*not* reproduce on our golden frames, so it is content-dependent and is not
asserted anywhere in this tool. Compression alone motivates everything here.)

So this reports four axes rather than one number, because no single statistic
separates the cases. Each axis is blind to something another one catches:

  diffuse amplitude   is everything slightly off?        FLIP mean, dE mean
  localised amplitude is anything badly off?             FLIP weighted median,
                                                         dE p99.9 / max
  structure           a region, or scattered?            erosion survival,
                                                         largest blob + bbox
  character           is it just a global tone shift?    affine fit, and the
                                                         residue after removing it

⚠️ **This is an instrument, never a gate, and never coverage.** It inherits the
blindness of whatever images it is fed: a perceptual score over a flat-shaded
corpus is exactly as blind to gouraud defects as that corpus is. It also knows
nothing about run-to-run stability -- feed it only frames `gs_oracle.py` has
already established as stable in both arms.

⚠️⚠️ **Never triage a HARDWARE-ORACLE divergence by perceptual severity.**
Against a console capture the question is "does our model of the silicon have a
hole", and a perceptually invisible divergence is routinely the *most*
informative kind -- it is small because it is a narrow structural gap, not
because it does not matter. Proof from this very campaign: the two `gs-dither`
software defects land squarely in this tool's own "subtle / QUANTISATION HAZE"
band, and fixing them is what exposed that the Tile renderer had been serving
CT32 dither natively with no matrix at all -- floor and native agreeing by being
wrong together, invisible to the corpus. Severity-ranking would have
deprioritised the most structurally load-bearing finding of the campaign.

The line is: use this to decide **whether a fix is worth moving goldens for**,
which is a user-facing cost/benefit question. Never to decide whether a
divergence is worth *understanding*. A benign-looking score is often measuring a
cancellation.

⚠️ **The structure axis is an ATTRIBUTION instrument, and it stays valid exactly
where the severity verdict does not** -- capture work included. "One coherent
region, or scattered pixels" is a fact about which mechanism produced a
difference, not a judgement about whether anyone minds it, so the ⚠️⚠️ above does
not reach it. The two shapes have disjoint causes: scattered single pixels are a
boundary condition sampled rarely -- a tie, a rounding edge, a filter crossover
-- while coherent runs are a draw-level defect, a whole primitive served by the
wrong path. Binding a number of the first shape to damage of the second is this
campaign's most expensive recurring error, and refusing it costs one `profile()`
call. Measured instance: the M3 perspective unlock's `gs-grad` divergence is ~1%
of pixels, all of them off by exactly 1/16 texel, and it was attributed to
Dirge's 22.78% corpus damage -- which is 91.3% in horizontal runs of eight or
more, median error 25 levels, one row damaged across all 597 of its columns.
Different shapes, therefore different mechanisms, and the attribution was wrong
before any of the arithmetic was checked.

**The companion method is ABLATION, and it outranks every statistic in this
file.** When a number needs an owner, disable one candidate mechanism and
re-score: the number moves or it does not, and no inference is required. That
same Dirge figure fell 22.78% -> 6.46% the moment mipmapped perspective
triangles were floored, naming 52 draws out of 1,943 native -- one run, and an
answer no amount of profiling the diff image could have produced. Reach for the
metrics here to *characterise* a difference; reach for ablation to *own* it.

Usage:
  gs_perceptual.py REF.png TEST.png [--json OUT] [--ppd N]
  gs_perceptual.py --calibrate ROOT [ROOT ...]   # sweep known-invisible pairs
  gs_perceptual.py --selftest ROOT               # injected-defect battery
  gs_perceptual.py --verify-flip PYTHON          # our FLIP vs NVIDIA's
"""

import argparse
import json
import os
import sys

SCRIPT_DIR = os.path.abspath(os.path.dirname(__file__))

# --- Calibration ----------------------------------------------------------
#
# Every threshold below is anchored on 108 pairs that are known to be invisible
# *by construction*: the same 9 dumps x 4 frames rendered by the software
# rasterizer on four different builds (the M2 dev box and three handhelds).
# They differ only by last-bit floating-point rounding -- never by more than 8
# levels, mostly +-1 on 2-7% of pixels. Anything that flags one of those 108 is
# miscalibrated, and `--calibrate` re-measures the headroom.
#
# ⚠️ Read this as a **false-positive floor and nothing else**. It certifies
# "differences of this size, arising this way, are benign" -- a claim about
# observed differences, never about correctness. Stated that way it cannot be
# undermined by the four roots agreeing for a bad reason, because their
# agreements are not evidence of anything and are not being used as such.
#
# ⚠️ It is structurally silent about false negatives. A defect that is
# deterministic in the shared code produces zero difference across all four
# roots, so it never enters the population at all -- the floor is not wrong
# about it, it simply has nothing to say. That is the same shape as three
# renderer arms scoring a capture byte-identically: agreement across instances
# of one codebase is a shared-code signature and carries no evidence about that
# code.
#
# ⚠️ It bounds FP-rounding noise *between builds of one renderer*. It is not a
# tile-vs-sw tolerance, and it is not a software-vs-console tolerance -- there
# is no reason the invisible floor for a cross-oracle population is last-bit
# anything. Do not reuse it as one.

JND = 2.3           # CIEDE2000 just-noticeable difference, the standard figure
DE_VISIBLE = 1.0    # below this, not perceptible even side by side

# Invisibility rests on three orthogonal criteria -- amplitude, extent and
# contiguity -- with the worst value the 108-pair floor actually reaches beside
# each (measured 2026-08-12; `--calibrate` re-measures at any time).
FLOOR = {
    "flip_mean": 0.010,      # amplitude.   floor reaches 0.0042  -> 2.4x headroom
    "above_jnd_pct": 0.05,   # extent.      floor reaches 0.0039  -> 12.8x
    "largest_blob_px": 64,   # contiguity.  floor reaches 2       -> 32x
}

# ⚠️ Three statistics we report but deliberately do NOT gate invisibility on,
# because each is *conditional on there being error* and so says nothing about
# how much of the frame is affected:
#
#   de_max                -- the benign floor reaches dE 7.996 on a single
#                            pixel. One pixel is never visible, so any
#                            max-based gate loose enough to pass the floor is
#                            too loose to mean anything.
#   flip_weighted_median  -- weights each pixel by its own error, so on a frame
#                            that is 4 pixels wrong and otherwise exact it
#                            reads ~0.03 and vetoes an obviously invisible
#                            result. (Found exactly that way: OutRun tile frame
#                            1, 4 px, FLIP mean 8e-06, called "NOTICEABLE
#                            BANDING" until this gate was fixed.) It is the
#                            right statistic for ranking *defects* and the
#                            wrong one for deciding whether there is one.
#   de_p99_9              -- redundant here anyway: p99.9 above the JND implies
#                            at least 0.1% of pixels above it, which the extent
#                            criterion already catches at 0.05%.
#
# All three still drive defect *detection* and severity below. The distinction
# is between "is anything wrong" (extent) and "how wrong is it" (tails).

# dE at which a *tail* stops being rounding and starts being a defect. The
# benign floor tops out at 7.996 and every injected defect clears 15.
DE_DEFECT = 15.0

# Levels apart at which a pixel is BADLY wrong -- a quarter of the range, far
# past anything rounding produces. dE is a colour *distance* and does not answer
# this: a 23-level shift on a saturated surface reads dE 59. Rogue Galaxy at 2x
# shipped as an "obvious localised defect" on a 23,065 px dE region whose worst
# pixel was 23 levels off, because nothing in the profile was measured in levels.
BLOCK64_LEVELS = 64

# Above this fraction of squared error explained by a per-channel gain+offset,
# the difference is a global tone transform rather than a defect.
AFFINE_GLOBAL = 0.95

SEVERITY_ORDER = ["invisible", "subtle", "noticeable", "obvious", "gross"]


# ---------------------------------------------------------------------------
# CIEDE2000
# ---------------------------------------------------------------------------

def srgb_to_lab(rgb):
    """sRGB u8/float -> CIELAB under D65."""
    import numpy as np

    c = np.asarray(rgb, dtype=np.float64)
    if np.asarray(rgb).dtype == np.uint8:
        c = c / 255.0
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4124564, 0.3575761, 0.1804375],
                  [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]])
    xyz = (lin @ m.T) / np.array([0.95047, 1.0, 1.08883])
    d = 6.0 / 29.0
    f = np.where(xyz > d ** 3, np.cbrt(np.maximum(xyz, 0.0)),
                 xyz / (3.0 * d * d) + 4.0 / 29.0)
    return np.stack([116.0 * f[..., 1] - 16.0,
                     500.0 * (f[..., 0] - f[..., 1]),
                     200.0 * (f[..., 1] - f[..., 2])], axis=-1)


def ciede2000(lab1, lab2):
    """Per-pixel CIEDE2000 (CIE 142-2001, per Sharma/Wu/Dalal 2005).

    Chosen over plain Euclidean CIE76 for one reason: its thresholds are
    standardised, so a number here means something outside this codebase.
    Under 1 is imperceptible, 1-2 needs close side-by-side inspection, and the
    JND is conventionally 2.3.
    """
    import numpy as np

    L1, a1, b1 = lab1[..., 0], lab1[..., 1], lab1[..., 2]
    L2, a2, b2 = lab2[..., 0], lab2[..., 1], lab2[..., 2]

    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2)
    Cbar = 0.5 * (C1 + C2)
    Cbar7 = Cbar ** 7
    G = 0.5 * (1.0 - np.sqrt(Cbar7 / (Cbar7 + 25.0 ** 7)))
    a1p, a2p = (1.0 + G) * a1, (1.0 + G) * a2
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)

    h1p = np.degrees(np.arctan2(b1, a1p)) % 360.0
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360.0
    # Hue is undefined at zero chroma; the standard's convention is to treat the
    # pair as having no hue difference there rather than to let arctan2(0,0)
    # manufacture one.
    zero1, zero2 = (C1p == 0), (C2p == 0)
    h1p = np.where(zero1, 0.0, h1p)
    h2p = np.where(zero2, 0.0, h2p)
    either_zero = zero1 | zero2

    dLp = L2 - L1
    dCp = C2p - C1p

    dhp = h2p - h1p
    dhp = np.where(dhp > 180.0, dhp - 360.0, dhp)
    dhp = np.where(dhp < -180.0, dhp + 360.0, dhp)
    dhp = np.where(either_zero, 0.0, dhp)
    dHp = 2.0 * np.sqrt(C1p * C2p) * np.sin(np.radians(dhp) / 2.0)

    Lbarp = 0.5 * (L1 + L2)
    Cbarp = 0.5 * (C1p + C2p)

    hsum, hdiff = h1p + h2p, np.abs(h1p - h2p)
    hbarp = np.where(either_zero, hsum,
                     np.where(hdiff <= 180.0, 0.5 * hsum,
                              np.where(hsum < 360.0, 0.5 * (hsum + 360.0),
                                       0.5 * (hsum - 360.0))))

    T = (1.0
         - 0.17 * np.cos(np.radians(hbarp - 30.0))
         + 0.24 * np.cos(np.radians(2.0 * hbarp))
         + 0.32 * np.cos(np.radians(3.0 * hbarp + 6.0))
         - 0.20 * np.cos(np.radians(4.0 * hbarp - 63.0)))

    dtheta = 30.0 * np.exp(-(((hbarp - 275.0) / 25.0) ** 2))
    Cbarp7 = Cbarp ** 7
    Rc = 2.0 * np.sqrt(Cbarp7 / (Cbarp7 + 25.0 ** 7))
    Lm50 = (Lbarp - 50.0) ** 2
    Sl = 1.0 + (0.015 * Lm50) / np.sqrt(20.0 + Lm50)
    Sc = 1.0 + 0.045 * Cbarp
    Sh = 1.0 + 0.015 * Cbarp * T
    Rt = -np.sin(np.radians(2.0 * dtheta)) * Rc

    return np.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2
                   + Rt * (dCp / Sc) * (dHp / Sh))


# ---------------------------------------------------------------------------
# Structure
# ---------------------------------------------------------------------------

def erosion_survival(mask):
    """Fraction of set pixels whose whole 3x3 neighbourhood is also set.

    The cheap clustering test, and a remarkably clean one: isolated pixels never
    survive an erosion, so scattered rounding noise scores *exactly* zero while
    an area defect scores 0.90-0.98. Blind to thin geometry by construction --
    that is what the magnitude tail is for.
    """
    import numpy as np

    n = int(mask.sum())
    if n == 0:
        return 0.0
    eroded = mask.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dy == 0 and dx == 0:
                continue
            shifted = np.roll(np.roll(mask, dy, axis=0), dx, axis=1)
            # Rolling wraps; a border pixel has no neighbour off-image, so treat
            # the wrapped-in edge as unset rather than letting it leak across.
            if dy == 1:
                shifted[0, :] = False
            elif dy == -1:
                shifted[-1, :] = False
            if dx == 1:
                shifted[:, 0] = False
            elif dx == -1:
                shifted[:, -1] = False
            eroded &= shifted
    return float(eroded.sum()) / n


def largest_component(mask):
    """Largest 4-connected component: (pixel count, bbox, total component count).

    Scanline union-find over the set pixels only, which keeps this to ~0.15 s on
    a full frame at worst and avoids a scipy dependency the rest of the harness
    does not have. bbox is (x0, y0, x1, y1), inclusive.
    """
    import numpy as np

    ys, xs = np.nonzero(mask)
    if len(ys) == 0:
        return 0, None, 0

    index = -np.ones(mask.shape, dtype=np.int64)
    index[ys, xs] = np.arange(len(ys))
    parent = np.arange(len(ys))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    up = index[ys - 1, xs]
    up[ys == 0] = -1
    left = index[ys, xs - 1]
    left[xs == 0] = -1
    for i, (u, l) in enumerate(zip(up, left)):
        for nb in (u, l):
            if nb >= 0:
                ra, rb = find(i), find(nb)
                if ra != rb:
                    parent[ra] = rb

    roots = np.array([find(i) for i in range(len(ys))])
    counts = np.bincount(roots)
    best = int(counts.argmax())
    sel = roots == best
    bx, by = xs[sel], ys[sel]
    return (int(counts[best]),
            (int(bx.min()), int(by.min()), int(bx.max()), int(by.max())),
            int((counts > 0).sum()))


def block64(ref_rgb, test_rgb):
    """Badly-wrong pixels: (count, largest 4-connected run, bbox of that run).

    A pixel counts when some colour channel is >= BLOCK64_LEVELS off. Alpha is
    excluded on purpose -- it is not directly visible, so it is counted
    elsewhere and never scored.

    The count says how much is badly wrong; the largest run says whether it is
    one thing. Katamari's Turnip blend bug is 16,519 px in ONE region, Stuntman's
    2x edge disagreement is 32,803 px in runs no bigger than 2,248 -- the same
    order of magnitude, two entirely different defects.
    """
    import numpy as np

    ref = np.asarray(ref_rgb)[..., :3].astype(np.int16)
    test = np.asarray(test_rgb)[..., :3].astype(np.int16)
    mask = np.abs(ref - test).max(axis=2) >= BLOCK64_LEVELS
    largest, bbox, _count = largest_component(mask)
    return int(mask.sum()), int(largest), (list(bbox) if bbox else None)


# ---------------------------------------------------------------------------
# Character: is the whole difference one global tone transform?
# ---------------------------------------------------------------------------

def affine_fit(ref, test):
    """Fit per-channel `test ~= gain*ref + offset`; report how much it explains.

    This is what separates a full-screen bloom or exposure change from a real
    defect, and it is decisive: measured on real frames it explains 100% of a
    global gain, 99% of a gamma tweak, and at most 2.5% of any localised bug.
    It is what lets the scorecard say "81.6% of pixels moved, all of it one
    gain" instead of reporting a catastrophe.
    """
    import numpy as np

    a = np.asarray(ref, dtype=np.float64)
    b = np.asarray(test, dtype=np.float64)
    total = residual = 0.0
    params = []
    for c in range(3):
        x, y = a[..., c].ravel(), b[..., c].ravel()
        design = np.stack([x, np.ones_like(x)], axis=1)
        coef, _, _, _ = np.linalg.lstsq(design, y, rcond=None)
        params.append((float(coef[0]), float(coef[1])))
        residual += float(((y - design @ coef) ** 2).sum())
        total += float(((y - x) ** 2).sum())
    if total <= 0.0:
        return 1.0, params, b.copy()
    explained = 1.0 - residual / total

    # The residue: the test image with its fitted global transform undone, so
    # the whole battery can be re-run on what the transform does *not* explain.
    corrected = np.empty_like(b)
    for c, (g, o) in enumerate(params):
        corrected[..., c] = np.clip((b[..., c] - o) / (g if g != 0.0 else 1.0),
                                    0.0, 255.0)
    return explained, params, corrected


# ---------------------------------------------------------------------------
# The battery
# ---------------------------------------------------------------------------

def _core_metrics(ref_rgb, test_rgb, ppd):
    """Everything except the affine/character axis (which recurses into this)."""
    import numpy as np
    import flip_numpy

    err, flip_stats = flip_numpy.flip(ref_rgb.astype(np.uint8),
                                      test_rgb.astype(np.uint8), ppd=ppd)
    de = ciede2000(srgb_to_lab(ref_rgb.astype(np.uint8)),
                   srgb_to_lab(test_rgb.astype(np.uint8)))

    mask = de > JND
    blob_px, bbox, blob_count = largest_component(mask)
    b64_px, b64_largest, b64_bbox = block64(ref_rgb, test_rgb)

    return {
        "flip_mean": round(flip_stats["mean"], 6),
        "flip_weighted_median": round(flip_stats["weighted_median"], 6),
        "flip_q3": round(flip_stats["weighted_q3"], 6),
        "flip_max": round(flip_stats["max"], 6),
        "de_mean": round(float(de.mean()), 4),
        # ⚠️ p99 is deliberately absent. A 2px sliver is 0.31% of a frame, so
        # p99 reads 0.00 and misses it completely. Percentile choice is
        # load-bearing here; p99.9 and max are the ones that see thin geometry.
        "de_p99_9": round(float(np.percentile(de, 99.9)), 4),
        "de_max": round(float(de.max()), 4),
        "above_jnd_pct": round(100.0 * float(mask.mean()), 4),
        "above_visible_pct": round(100.0 * float((de > DE_VISIBLE).mean()), 4),
        "erosion_survival": round(erosion_survival(mask), 4),
        "largest_blob_px": blob_px,
        "largest_blob_bbox": bbox,
        "blob_count": blob_count,
        # ⚠️ Not the same thing as largest_blob_px, and the gap is the point:
        # largest_blob_px is the AREA above the JND, these are the pixels that
        # are badly wrong. On real cells they differ by three orders of magnitude.
        "block64_px": b64_px,
        "block64_largest_px": b64_largest,
        "block64_bbox": b64_bbox,
    }, err, de


def profile(ref_rgb, test_rgb, ppd=None):
    """Full four-axis perceptual profile of `test_rgb` against `ref_rgb`.

    Both are HxWx3 uint8. Returns the metric block; pass it to `classify`.
    """
    import numpy as np
    import flip_numpy

    if ppd is None:
        ppd = flip_numpy.HANDHELD_PPD

    ref_rgb = np.asarray(ref_rgb)
    test_rgb = np.asarray(test_rgb)
    if ref_rgb.shape != test_rgb.shape:
        raise ValueError(f"shape mismatch: {ref_rgb.shape} vs {test_rgb.shape}")

    block, _, _ = _core_metrics(ref_rgb, test_rgb, ppd)
    block["ppd"] = round(float(ppd), 4)

    explained, params, corrected = affine_fit(ref_rgb, test_rgb)
    block["affine_explained"] = round(float(explained), 4)
    block["affine_gain"] = [round(g, 5) for g, _ in params]
    block["affine_offset"] = [round(o, 4) for _, o in params]

    # Only worth the second pass when the transform actually explains something;
    # otherwise the residue is the original difference and says nothing new.
    # Kept whole rather than trimmed, because `classify` runs on it recursively.
    if explained >= 0.5:
        residue, _, _ = _core_metrics(ref_rgb, corrected, ppd)
        block["residue"] = residue
    else:
        block["residue"] = None

    return block


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------

# Above this many separate blobs, an unclustered defect is scatter rather than
# one thin run -- a different diagnosis, so a different name.
SPECKLE_BLOBS = 8

STRUCTURAL = {"THIN GEOMETRY", "SPECKLE", "LOCALISED DEFECT",
              "WRONG BLEND / TEXTURE", "GROSS CORRUPTION"}


def _is_invisible(b):
    return all(b.get(k, 0.0) <= v for k, v in FLOOR.items())


def classify(block):
    """Reduce the battery to (severity, verdict, detail).

    The verdict names a defect *class*, not just a magnitude, because a bbox
    tells you where to look and a scalar does not.
    """
    if block.get("identical"):
        return "invisible", "IDENTICAL", "byte-identical"

    if _is_invisible(block):
        return ("invisible", "INVISIBLE",
                f"worst dE {block['de_max']:.1f}, "
                f"{block['above_jnd_pct']:.2f}% above JND")

    # A global tone transform that hides nothing structural is a different
    # animal from a defect, however many pixels it moved -- this is the axis
    # that turns "81.6% of pixels wrong" into "one gain of 1.06".
    #
    # The residue decides it, and it is classified by *this same function*: a
    # tone shift is only exonerated when what it fails to explain would not
    # itself be called a defect. Clipping in the highlights is diffuse and
    # passes; a dropped polygon underneath a bloom change is structural and
    # demotes the whole frame to that defect.
    residue = block.get("residue")
    if block.get("affine_explained", 0.0) >= AFFINE_GLOBAL and residue is not None:
        _, residue_verdict, _ = classify(dict(residue, affine_explained=0.0,
                                              residue=None))
        if residue_verdict not in STRUCTURAL:
            gains = block["affine_gain"]
            spread = max(gains) - min(gains)
            kind = "colour cast" if spread > 0.01 else "exposure/gain shift"
            # FLIP's own published fail threshold is a mean of 0.05.
            sev = "subtle" if block["flip_mean"] < 0.05 else "noticeable"
            return (sev, "GLOBAL TONE SHIFT",
                    f"{kind}, per-channel gain {gains}, explains "
                    f"{100 * block['affine_explained']:.1f}% of the error; "
                    f"residue is {residue_verdict.lower()}, not structural")

    clustered = block["erosion_survival"] >= 0.5
    big_tail = block["de_max"] > DE_DEFECT or block["de_p99_9"] > DE_DEFECT
    frame_fraction = block["above_jnd_pct"]

    # Erosion survival at ~0 means nothing in the wrong region is more than a
    # pixel or two thick. That IS the thinness test -- do not also bound the
    # pixel count, because a seam down a 448-line frame is 896 px and thin.
    #
    # But thin-and-connected is a different defect from thin-and-everywhere. A
    # seam is one run and points at a rasterizer edge case; hundreds of
    # fragments scattered over the frame is sampling or filtering disagreement.
    # Both want investigating, they just want investigating by different people,
    # so they get different names. (Classic-vs-SW on Shadow of the Colossus is
    # the second kind: 511 fragments, largest 63 px.)
    if big_tail and not clustered:
        sev = "obvious" if block["flip_weighted_median"] > 0.3 else "noticeable"
        if block["blob_count"] <= SPECKLE_BLOBS:
            return (sev, "THIN GEOMETRY",
                    f"sliver or seam in {block['blob_count']} run(s): worst dE "
                    f"{block['de_max']:.1f} over {frame_fraction:.2f}% of the "
                    f"frame, largest blob {block['largest_blob_px']} px "
                    f"at {block['largest_blob_bbox']}")
        return (sev, "SPECKLE",
                f"{block['blob_count']} scattered fragments, largest "
                f"{block['largest_blob_px']} px: worst dE {block['de_max']:.1f} "
                f"over {frame_fraction:.2f}% of the frame, unclustered "
                f"(erosion {block['erosion_survival']:.3f}) -- reads as sampling "
                f"or filtering disagreement rather than one broken primitive")

    # "Missing or extra geometry" is a claim that something is WRONG, not merely
    # a different colour, so it has to be earned in levels and not only in dE.
    # A clustered dE region whose worst pixel is 23 levels off is a shading
    # difference; calling it a localised defect is how Rogue Galaxy's faint sash
    # gradient was handed over as the corpus's second-worst cell. A block that
    # never measured block64 (an older cached profile) does not get vetoed.
    b64 = block.get("block64_largest_px")
    badly_wrong = b64 is None or b64 >= FLOOR["largest_blob_px"]
    if clustered and big_tail and frame_fraction < 5.0 and badly_wrong:
        core = (f"{block['block64_px']} px at 64+ levels (largest run {b64} px), "
                if b64 is not None else "")
        return ("obvious", "LOCALISED DEFECT",
                f"missing or extra geometry: {core}dE region "
                f"{block['largest_blob_px']} px at {block['largest_blob_bbox']}, "
                f"worst dE {block['de_max']:.1f}")

    if clustered and frame_fraction >= 5.0:
        sev = "gross" if (block["flip_mean"] > 0.08 and block["de_mean"] > 4.0) else "obvious"
        name = "GROSS CORRUPTION" if sev == "gross" else "WRONG BLEND / TEXTURE"
        return (sev, name,
                f"{frame_fraction:.1f}% of the frame above JND, clustered "
                f"(erosion {block['erosion_survival']:.3f}), "
                f"largest blob {block['largest_blob_px']} px "
                f"at {block['largest_blob_bbox']}")

    # Whatever is left is diffuse and unclustered: quantisation-shaped. Banding
    # separates from dither haze by amplitude, not by structure -- but "diffuse"
    # is a claim about extent, so it has to be earned. A frame that is four
    # pixels wrong is not banding however wrong those four pixels are.
    # ⚠️ STRAY PIXELS and SPECKLE are deliberately different verdicts, and only
    # SPECKLE is structural. Both are unclustered scatter, but this branch is
    # reached only when the extent is below the invisibility floor -- too little
    # of the frame to see. Naming them the same thing once let a global tone
    # shift's own clipping residue veto the tone-shift verdict.
    if frame_fraction < FLOOR["above_jnd_pct"]:
        return ("subtle", "STRAY PIXELS",
                f"a handful of wrong pixels ({frame_fraction:.4f}% above JND, "
                f"largest blob {block['largest_blob_px']} px at "
                f"{block['largest_blob_bbox']}, worst dE {block['de_max']:.1f}) "
                f"-- too little of the frame to see, but not nothing")
    if block["de_max"] > 4.0 or block["flip_mean"] > 0.05:
        return ("noticeable", "BANDING",
                f"diffuse quantisation: {frame_fraction:.1f}% above JND, "
                f"worst dE {block['de_max']:.1f}, FLIP mean {block['flip_mean']:.4f}")
    return ("subtle", "QUANTISATION HAZE",
            f"diffuse low-amplitude: worst dE {block['de_max']:.1f}, "
            f"{frame_fraction:.2f}% above JND, unclustered")


def summarize(block):
    sev, verdict, detail = classify(block)
    if verdict == sev.upper():
        return f"{verdict} -- {detail}"
    return f"{sev.upper()}: {verdict} -- {detail}"


# ---------------------------------------------------------------------------
# Image loading
# ---------------------------------------------------------------------------

def load_rgb(path):
    """PNG -> HxWx3 uint8, plus the alpha plane if the file carried one."""
    import numpy as np
    from PIL import Image

    img = Image.open(path)
    alpha = None
    if img.mode == "RGBA":
        arr = np.asarray(img)
        alpha = arr[..., 3]
        rgb = arr[..., :3]
    else:
        rgb = np.asarray(img.convert("RGB"))
    return np.ascontiguousarray(rgb), alpha


def compare_files(ref_path, test_path, ppd=None, allow_resize=False):
    import numpy as np

    ref, ref_a = load_rgb(ref_path)
    test, test_a = load_rgb(test_path)

    if ref.shape != test.shape:
        if not allow_resize:
            raise SystemExit(
                f"resolution mismatch: {ref.shape[1]}x{ref.shape[0]} vs "
                f"{test.shape[1]}x{test.shape[0]}. These are different "
                f"measurements, not a difference -- pass --allow-resize only if "
                f"you intend to score an upscaled arm against a native golden.")
        from PIL import Image
        test = np.asarray(Image.fromarray(test).resize(
            (ref.shape[1], ref.shape[0]), Image.BOX))
        test_a = None

    if np.array_equal(ref, test):
        block = {"identical": True, "flip_mean": 0.0,
                 "flip_weighted_median": 0.0, "de_max": 0.0, "de_p99_9": 0.0,
                 "above_jnd_pct": 0.0, "erosion_survival": 0.0,
                 "largest_blob_px": 0, "largest_blob_bbox": None}
    else:
        block = profile(ref, test, ppd=ppd)
        block["identical"] = False

    # Alpha is not directly visible, so it gets counted, never scored.
    if ref_a is not None and test_a is not None and ref_a.shape == test_a.shape:
        d = np.abs(ref_a.astype(np.int16) - test_a.astype(np.int16))
        block["alpha"] = {"pct_differing": round(100.0 * float((d > 0).mean()), 4),
                          "max_diff": int(d.max())}

    sev, verdict, detail = classify(block)
    block["severity"] = sev
    block["verdict"] = verdict
    block["detail"] = detail
    return block


# ---------------------------------------------------------------------------
# calibrate
# ---------------------------------------------------------------------------

def _golden_frames(root):
    import glob as _glob
    out = {}
    for manifest in _glob.glob(os.path.join(_glob.escape(root), "*", "manifest.json")):
        d = os.path.dirname(manifest)
        for png in sorted(_glob.glob(os.path.join(_glob.escape(d), "frames", "*.png"))):
            out[(os.path.basename(d), os.path.basename(png))] = png
    return out


def cmd_calibrate(args):
    """Re-measure the invisible floor over pairs known to be benign.

    The four golden roots hold the same frames rendered by the *software*
    rasterizer on four different builds. Every difference between them is
    last-bit FP rounding, so every pair must come back INVISIBLE. If one does
    not, the thresholds are wrong -- not the renderer.
    """
    roots = args.calibrate
    if len(roots) < 2:
        raise SystemExit("--calibrate needs at least two golden roots to compare")

    base = _golden_frames(roots[0])
    if not base:
        raise SystemExit(f"no golden frames under {roots[0]}")

    worst = {}
    pairs = flagged = identical = 0
    for other in roots[1:]:
        theirs = _golden_frames(other)
        for key, ref_path in sorted(base.items()):
            test_path = theirs.get(key)
            if test_path is None:
                continue
            block = compare_files(ref_path, test_path, ppd=args.ppd)
            pairs += 1
            if block.get("identical"):
                identical += 1
                continue
            for k in FLOOR:
                if k in block:
                    worst[k] = max(worst.get(k, 0.0), block[k])
            if block["severity"] != "invisible":
                flagged += 1
                print(f"  FLAGGED {block['severity']:10} {block['verdict']:20} "
                      f"{key[0][:28]:30} {key[1][-13:-4]} vs {os.path.basename(other)}")
                print(f"          {block['detail']}")

    if pairs == 0:
        raise SystemExit("no comparable pairs found across the given roots -- "
                         "check the roots hold the same dumps")

    print(f"\ncalibration over {pairs} known-invisible pairs "
          f"({identical} byte-identical, {pairs - identical} differing):")
    for k in sorted(FLOOR):
        measured = worst.get(k, 0.0)
        head = FLOOR[k] / measured if measured else float("inf")
        mark = "  " if measured <= FLOOR[k] else "!!"
        print(f"  {mark} {k:22} worst {measured:10.4f}   floor {FLOOR[k]:8.3f}"
              f"   headroom {head:6.2f}x" if measured else
              f"     {k:22} worst {measured:10.4f}   floor {FLOOR[k]:8.3f}")
    print(f"\n{'PASS' if flagged == 0 else 'FAIL'}: {flagged} of {pairs} "
          f"known-invisible pairs were flagged as visible")
    return 0 if flagged == 0 else 1


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------

def _pick_golden(root):
    import glob as _glob
    for manifest in sorted(_glob.glob(os.path.join(_glob.escape(root), "*",
                                                   "manifest.json"))):
        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        for idx, stable in sorted(m["stable"].items(), key=lambda kv: int(kv[0])):
            if stable:
                return os.path.join(os.path.dirname(manifest), "frames",
                                    m["frame_files"][idx])
    return None


def _mutations(golden):
    """One injected defect per class in the taxonomy, on a real frame."""
    import numpy as np

    h, w, _ = golden.shape
    rng = np.random.default_rng(0xA125)  # deterministic: a selftest must not flake
    out = {}

    lsb = golden.astype(np.int16) + (rng.random((h, w, 1)) < 0.07) * rng.integers(-1, 2, golden.shape)
    out["lsb_sparse"] = ("invisible", None, np.clip(lsb, 0, 255).astype(np.uint8))

    haze = golden.astype(np.int16) + rng.integers(-1, 2, golden.shape)
    out["dither_haze"] = ("subtle", "QUANTISATION HAZE", np.clip(haze, 0, 255).astype(np.uint8))

    # A 6% exposure shift is a real, seeable change -- "noticeable" is the
    # honest severity. What matters is that it is named a tone shift and not a
    # defect, which is the Shadow of the Colossus case.
    out["global_gain"] = ("noticeable", "GLOBAL TONE SHIFT",
                          np.clip(golden.astype(np.float64) * 1.06, 0, 255).astype(np.uint8))

    tint = golden.astype(np.float64).copy()
    tint[..., 2] = np.clip(tint[..., 2] * 1.15, 0, 255)
    out["blue_tint"] = (None, "GLOBAL TONE SHIFT", tint.astype(np.uint8))

    out["banding_16bpp"] = (None, "BANDING", (golden & 0xF8))

    seam = golden.copy()
    seam[:, w // 2:w // 2 + 2] = 255
    out["thin_seam"] = (None, "THIN GEOMETRY", seam)

    patch = golden.copy()
    y0, x0 = h // 3, w // 3
    patch[y0:y0 + 64, x0:x0 + 64] = 0
    out["missing_patch"] = ("obvious", "LOCALISED DEFECT", patch)

    corrupt = golden.copy()
    corrupt[h // 4:3 * h // 4, w // 4:3 * w // 4] = rng.integers(
        0, 256, (h // 2, 3 * w // 4 - w // 4, 3), dtype=np.uint8)
    out["gross_corruption"] = (None, None, corrupt)

    return out


def cmd_selftest(args):
    """Prove each axis discriminates, on injected defects in a real frame."""
    import numpy as np

    path = _pick_golden(args.selftest)
    if path is None:
        raise SystemExit("selftest needs a golden with a stable frame; run "
                         "`gs_oracle.py golden` first")
    golden, _ = load_rgb(path)
    h, w, _ = golden.shape
    print(f"selftest against {os.path.basename(path)} ({w}x{h})\n")

    failures = []

    def check(name, ok, detail):
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + ("" if ok else f": {detail}"))
        if not ok:
            failures.append(name)

    identical = profile(golden, golden.copy(), ppd=args.ppd)
    sev, verdict, _ = classify(identical)
    check("an identical copy is invisible", sev == "invisible",
          f"got {sev}/{verdict}")

    results = {}
    print(f"\n  {'case':20}{'sev':11}{'verdict':22}{'FLIPmean':>10}"
          f"{'FLIPwmed':>10}{'dEmax':>8}{'eros':>7}{'blob':>8}{'affine':>8}")
    for name, (want_sev, want_verdict, mutated) in _mutations(golden).items():
        b = profile(golden, mutated, ppd=args.ppd)
        sev, verdict, _ = classify(b)
        results[name] = (sev, verdict, b)
        print(f"  {name:20}{sev:11}{verdict:22}{b['flip_mean']:10.5f}"
              f"{b['flip_weighted_median']:10.5f}{b['de_max']:8.2f}"
              f"{b['erosion_survival']:7.3f}{b['largest_blob_px']:8d}"
              f"{b['affine_explained']:8.3f}")

    print()
    for name, (want_sev, want_verdict, _m) in _mutations(golden).items():
        sev, verdict, _b = results[name]
        if want_sev is not None:
            check(f"{name} severity is {want_sev}", sev == want_sev, f"got {sev}")
        if want_verdict is not None:
            check(f"{name} classifies as {want_verdict}", verdict == want_verdict,
                  f"got {verdict}")

    # ---- The reason this tool exists -------------------------------------
    #
    # ⚠️ The failure of `mean_diff` is COMPRESSION, not inversion. It is
    # tempting to claim the old metric ranks a defect below benign noise, and
    # on some content it does -- but measured here it does not, and a selftest
    # must assert what is true on the frame in front of it. What it reliably
    # does is squash a catastrophic defect and an invisible haze into the same
    # narrow band, which is just as fatal for a threshold and is robust.
    muts = _mutations(golden)

    def mean_diff(name):
        return float(np.abs(golden.astype(np.int16)
                            - muts[name][2].astype(np.int16)).mean())

    check("a wrong patch outranks a dither haze perceptually",
          SEVERITY_ORDER.index(results["missing_patch"][0])
          > SEVERITY_ORDER.index(results["dither_haze"][0]),
          f"patch={results['missing_patch'][0]} haze={results['dither_haze'][0]}")

    # A seam of dE 100 against an invisible haze is the sharpest pair: the two
    # are indistinguishable to a mean and 80x apart perceptually.
    seam_mean, haze_mean = mean_diff("thin_seam"), mean_diff("dither_haze")
    seam_wmed = results["thin_seam"][2]["flip_weighted_median"]
    haze_wmed = results["dither_haze"][2]["flip_weighted_median"]
    mean_ratio = seam_mean / haze_mean
    wmed_ratio = seam_wmed / max(haze_wmed, 1e-9)

    check("mean_diff CANNOT separate a dE-100 seam from an invisible haze "
          "(the defect being fixed)", mean_ratio < 1.5,
          f"ratio {mean_ratio:.2f} -- if this now separates them, the premise changed")
    check("...while the perceptual statistic separates them by over 10x",
          wmed_ratio > 10.0, f"ratio {wmed_ratio:.1f}")
    print(f"        mean_diff:  seam {seam_mean:.4f} vs haze {haze_mean:.4f} "
          f"-> {mean_ratio:.2f}x apart (indistinguishable)")
    print(f"        FLIP wmed:  seam {seam_wmed:.4f} vs haze {haze_wmed:.4f} "
          f"-> {wmed_ratio:.0f}x apart (decisive)")

    print(f"\nselftest: {'PASS' if not failures else 'FAIL (' + ', '.join(failures) + ')'}")
    return 0 if not failures else 1


# ---------------------------------------------------------------------------
# verify-flip
# ---------------------------------------------------------------------------

def cmd_verify_flip(args):
    """Re-check our FLIP port against NVIDIA's, using a venv that has theirs.

    Build one with `pip install flip-evaluator numpy pillow` -- it compiles from
    the sdist on aarch64 in about a minute. It is a development-time oracle
    only; nothing at runtime may depend on it.
    """
    import subprocess
    import glob as _glob

    frames = []
    for root in args.roots or [os.path.expanduser("~/gs-oracle/golden")]:
        frames.extend(sorted(_glob.glob(os.path.join(
            _glob.escape(root), "*", "frames", "*.png"))))
    if len(frames) < 2:
        raise SystemExit("need golden frames to compare; pass --roots")

    script = r'''
import sys, json
import numpy as np
from PIL import Image
sys.path.insert(0, sys.argv[1])
import flip_numpy, flip_evaluator
worst_map = worst_stat = 0.0
pairs = json.loads(sys.argv[2])
for a, b in pairs:
    A = np.asarray(Image.open(a).convert("RGB"), dtype=np.uint8)
    B = np.asarray(Image.open(b).convert("RGB"), dtype=np.uint8)
    ours, stats = flip_numpy.flip(A, B, ppd=flip_numpy.DEFAULT_PPD)
    theirs, refmean, _ = flip_evaluator.evaluate(a, b, "LDR", applyMagma=False)
    theirs = np.asarray(theirs, dtype=np.float64)
    if theirs.ndim == 3:
        theirs = theirs[..., 0]
    worst_map = max(worst_map, float(np.abs(ours - theirs).max()))
    worst_stat = max(worst_stat, abs(stats["mean"] - float(refmean)))
print(json.dumps({"pairs": len(pairs), "worst_map": worst_map,
                  "worst_mean": worst_stat}))
'''
    pairs = [[frames[i], frames[i + 1]] for i in range(0, len(frames) - 1, 2)]
    pairs = pairs[:args.limit]
    out = subprocess.run([args.verify_flip, "-c", script, SCRIPT_DIR,
                          json.dumps(pairs)], capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        raise SystemExit("reference run failed -- is flip_evaluator installed "
                         "in that interpreter?")
    r = json.loads(out.stdout.strip().splitlines()[-1])
    print(f"our FLIP vs NVIDIA's reference over {r['pairs']} pairs:")
    print(f"  worst per-pixel map difference: {r['worst_map']:.3e}")
    print(f"  worst pooled mean difference:   {r['worst_mean']:.3e}")
    ok = r["worst_map"] < 1e-3 and r["worst_mean"] < 1e-3
    print("PASS" if ok else "FAIL: divergence beyond float32 rounding")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("images", nargs="*", metavar="IMAGE",
                   help="REFERENCE.png TEST.png")
    p.add_argument("--ppd", type=float, default=None,
                   help="pixels per degree; default is the handheld viewing "
                        "model (~96). Scores are only comparable at a fixed ppd.")
    p.add_argument("--json", default=None, help="write the metric block here")
    p.add_argument("--allow-resize", action="store_true",
                   help="box-downsample the test image to the reference size")
    p.add_argument("--quiet", action="store_true", help="verdict line only")
    p.add_argument("--calibrate", nargs="+", metavar="ROOT",
                   help="re-measure the invisible floor across golden roots")
    p.add_argument("--selftest", metavar="ROOT",
                   help="run the injected-defect battery against a golden root")
    p.add_argument("--verify-flip", metavar="PYTHON",
                   help="path to a python with flip_evaluator installed")
    p.add_argument("--roots", nargs="+", help="golden roots for --verify-flip")
    p.add_argument("--limit", type=int, default=12,
                   help="max pairs for --verify-flip (default 12)")
    args = p.parse_args()

    if args.calibrate:
        return cmd_calibrate(args)
    if args.selftest:
        return cmd_selftest(args)
    if args.verify_flip:
        return cmd_verify_flip(args)

    if len(args.images) != 2:
        p.error("give exactly two images, or use --calibrate/--selftest/--verify-flip")

    block = compare_files(args.images[0], args.images[1], ppd=args.ppd,
                          allow_resize=args.allow_resize)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(block, f, indent=2)

    print(summarize(block))
    if not args.quiet and not block.get("identical"):
        print(f"  FLIP     mean {block['flip_mean']:.5f}  "
              f"weighted median {block['flip_weighted_median']:.5f}  "
              f"max {block['flip_max']:.5f}   (ppd {block['ppd']})")
        print(f"  dE2000   mean {block['de_mean']:.3f}  "
              f"p99.9 {block['de_p99_9']:.2f}  max {block['de_max']:.2f}  "
              f"above JND {block['above_jnd_pct']:.3f}%")
        print(f"  structure erosion survival {block['erosion_survival']:.3f}  "
              f"largest dE blob {block['largest_blob_px']} px at "
              f"{block['largest_blob_bbox']}  ({block['blob_count']} blobs)")
        if block.get("block64_px") is not None:
            print(f"  badly wrong {block['block64_px']} px at 64+ levels, "
                  f"largest run {block['block64_largest_px']} px at "
                  f"{block['block64_bbox']}")
        print(f"  character affine explains "
              f"{100 * block['affine_explained']:.1f}%  gain {block['affine_gain']}")
        if block.get("residue"):
            r = block["residue"]
            print(f"  residue  after removing it: FLIP mean {r['flip_mean']:.5f}, "
                  f"dE max {r['de_max']:.2f}, largest blob {r['largest_blob_px']} px")
        if block.get("alpha"):
            print(f"  alpha    {block['alpha']['pct_differing']:.3f}% differ, "
                  f"max {block['alpha']['max_diff']} (counted, not scored)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
