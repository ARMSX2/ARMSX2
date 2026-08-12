# SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
# SPDX-License-Identifier: GPL-3.0+
#
# This module is a NumPy transcription of the LDR-FLIP algorithm. The algorithm,
# its constants and its reference implementation are:
#
#   Copyright (c) 2020-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#   SPDX-License-Identifier: BSD-3-Clause
#   https://github.com/NVlabs/flip
#
#   Pontus Andersson, Jim Nilsson, Tomas Akenine-Moller, Magnus Oskarsson,
#   Kalle Astrom, Mark D. Fairchild, "FLIP: A Difference Evaluator for
#   Alternating Images", Proc. ACM Comput. Graph. Interact. Tech. 3(2), 2020
#   (HPG 2020).
#
# BSD-3-Clause is GPL-compatible; NVIDIA's notice is retained above as it
# requires. We transcribe rather than depend because `flip-evaluator` ships no
# aarch64 Linux wheel, and the oracle harness must run on the handhelds with
# nothing but numpy.

"""LDR-FLIP perceptual image difference, in pure NumPy.

FLIP models what a human notices when *flipping* back and forth between two
renderings of the same scene, which is literally what screenshot review is. It
beats generic image-quality metrics here for one specific reason: it carries a
separate **feature** channel driven by edge and point detectors, so a two-pixel
geometry sliver registers as a feature event instead of being averaged away as a
rounding error.

    err, stats = flip(reference_rgb_u8, test_rgb_u8, ppd=...)

`err` is the per-pixel error map in [0, 1]; `stats` carries the pooled numbers.

⚠️ **Use the weighted median, not the mean.** NVIDIA's current guidance
recommends the mean, superseding their own paper -- that advice targets
correlation with human opinion scores across broad distortion levels, and it is
actively wrong for defect detection. Measured on a PS2-like scene, mean FLIP
ranks a missing polygon (0.0063) as *better* than benign +-1 LSB rounding noise
(0.0138), because a mean can never be dominated by a handful of catastrophic
pixels. The weighted median puts benign cases at 0.015-0.023 and every localised
defect at 0.55-0.96. Keep the mean anyway: it is the informative statistic for
*diffuse* defects such as banding, where both rise together.

⚠️ **Pixels per degree is part of the measurement.** FLIP's default viewing model
is a 4K display at 0.7 m (~67 ppd), which is wrong for a 640x448 PS2 buffer
upscaled on a handheld. Scores are only comparable within a fixed ppd, so the
caller passes one and the harness records it alongside the score.

**Agreement with NVIDIA's reference.** Measured 2026-08-12 against
`flip-evaluator` 1.7 built from its sdist on this aarch64 box, over 16 pairs (8
injected defect classes spanning the taxonomy, plus 8 real golden frames):
worst per-pixel error-map difference **5.9e-05**, worst pooled-statistic
difference **1.8e-04**. That residue is float32-vs-float64 rounding -- the
reference computes in float32, we compute in float64. Re-checkable at any time
with `gs_perceptual.py --verify-flip <venv-python>`; the reference is never a
runtime dependency.

Cost is ~210 ms for a 597x448 pair (the C++ reference does it in 35 ms). A
36-frame corpus scores in well under ten seconds, so the gap does not matter.
"""

import numpy as np

# ---------------------------------------------------------------------------
# Constants (FLIP.h: xFLIPConstants, xGaussianConstants, DEFAULT_ILLUMINANT)
# ---------------------------------------------------------------------------

GQC = 0.7      # exponent applied to the HyAB colour distance
GPC = 0.4      # fraction of cmax below which error is mapped linearly into [0, gpt]
GPT = 0.95     # the knee of that remap
GW = 0.082     # feature-filter width in degrees
GQF = 0.5      # exponent applied to the feature difference

# Sum-of-Gaussians contrast sensitivity, one row per opponent channel.
A1 = (1.0, 1.0, 34.1)
B1 = (0.0047, 0.0053, 0.04)
A2 = (0.0, 0.0, 13.5)
B2 = (1.0e-5, 1.0e-5, 0.025)

ILLUMINANT = np.array([0.950428545, 1.0, 1.088900371])
INV_ILLUMINANT = np.array([1.052156925, 1.0, 0.918357670])

# FLIP's own matrices, kept to its exact rational/decimal forms so our numbers
# track the reference rather than a textbook sRGB matrix.
_RGB2XYZ = np.array([
    [10135552.0 / 24577794.0, 8788810.0 / 24577794.0, 4435075.0 / 24577794.0],
    [2613072.0 / 12288897.0, 8788810.0 / 12288897.0, 887015.0 / 12288897.0],
    [1425312.0 / 73733382.0, 8788810.0 / 73733382.0, 70074185.0 / 73733382.0],
])
_XYZ2RGB = np.array([
    [3.241003275, -1.537398934, -0.498615861],
    [-0.969224334, 1.875930071, 0.041554224],
    [0.055639423, -0.204011202, 1.057148933],
])

DEFAULT_PPD = 0.7 * (3840.0 / 0.7) * (np.pi / 180.0)  # ~67.02, FLIP's default

# A PS2 framebuffer on a 5.5" 1920x1080 handheld held at ~35 cm. Nothing about
# FLIP's default 4K-desktop geometry describes how these games are actually
# looked at, and ppd changes the answer, so the harness commits to a number.
HANDHELD_PPD = 0.35 * (1920.0 / 0.1219) * (np.pi / 180.0)  # ~96.4


# ---------------------------------------------------------------------------
# Colour space
# ---------------------------------------------------------------------------

def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _matmul(img, m):
    return img @ m.T


def linear_rgb_to_xyz(rgb):
    return _matmul(rgb, _RGB2XYZ)


def xyz_to_linear_rgb(xyz):
    return _matmul(xyz, _XYZ2RGB)


def xyz_to_ycxcz(xyz):
    n = xyz * INV_ILLUMINANT
    return np.stack([116.0 * n[..., 1] - 16.0,
                     500.0 * (n[..., 0] - n[..., 1]),
                     200.0 * (n[..., 1] - n[..., 2])], axis=-1)


def ycxcz_to_xyz(ycxcz):
    y = (ycxcz[..., 0] + 16.0) / 116.0
    cx = ycxcz[..., 1] / 500.0
    cz = ycxcz[..., 2] / 200.0
    return np.stack([y + cx, y, y - cz], axis=-1) * ILLUMINANT


def xyz_to_cielab(xyz):
    delta = 6.0 / 29.0
    delta3 = delta ** 3
    factor = 1.0 / (3.0 * delta * delta)
    term = 4.0 / 29.0
    n = xyz * INV_ILLUMINANT
    f = np.where(n > delta3, np.cbrt(np.maximum(n, 0.0)), factor * n + term)
    return np.stack([116.0 * f[..., 1] - 16.0,
                     500.0 * (f[..., 0] - f[..., 1]),
                     200.0 * (f[..., 1] - f[..., 2])], axis=-1)


def _hunt(lab):
    """Chroma scales with lightness: an error in a dark region matters less."""
    out = lab.copy()
    out[..., 1] = 0.01 * lab[..., 0] * lab[..., 1]
    out[..., 2] = 0.01 * lab[..., 0] * lab[..., 2]
    return out


def _hyab(a, b):
    """Hybrid distance: city-block in L, Euclidean in the chroma plane."""
    return (np.abs(a[..., 0] - b[..., 0])
            + np.hypot(a[..., 1] - b[..., 1], a[..., 2] - b[..., 2]))


def _max_distance():
    """HyAB between the most distant pair FLIP normalises against (green/blue)."""
    lab = xyz_to_cielab(linear_rgb_to_xyz(np.array([[[0.0, 1.0, 0.0],
                                                     [0.0, 0.0, 1.0]]])))
    hunted = _hunt(lab)
    return float(_hyab(hunted[:, 0], hunted[:, 1])[0]) ** GQC


# ---------------------------------------------------------------------------
# Separable convolution with clamp-to-edge, matching FLIP's boundary handling
# ---------------------------------------------------------------------------

def _pad_edge(a, r, axis):
    return np.pad(a, [(r, r) if i == axis else (0, 0) for i in range(a.ndim)],
                  mode="edge")


def _convolve1d(a, kernel, axis):
    """Correlate `a` with a 1-D `kernel` along `axis`, clamping at the edges.

    Implemented as a sum of shifted slices: at our radii this is a few dozen
    vectorised adds, which beats anything we could reach without scipy.
    """
    r = len(kernel) // 2
    padded = _pad_edge(a, r, axis)
    n = a.shape[axis]
    out = np.zeros_like(a, dtype=np.float64)
    for i, w in enumerate(kernel):
        if w == 0.0:
            continue
        sl = [slice(None)] * a.ndim
        sl[axis] = slice(i, i + n)
        out += w * padded[tuple(sl)]
    return out


# ---------------------------------------------------------------------------
# Filters
# ---------------------------------------------------------------------------

def _gaussian_ab(x2, a, b):
    return a * np.sqrt(np.pi / b) * np.exp(-(np.pi ** 2) * x2 / b)


def _gaussian_sqrt_ab(x2, a, b):
    return np.sqrt(a * np.sqrt(np.pi / b)) * np.exp(-(np.pi ** 2) * x2 / b)


def spatial_filter_radius(ppd):
    max_b = max(B1[0], B1[1], B1[2], B2[0], B2[1], B2[2])
    return int(np.ceil(3.0 * np.sqrt(max_b / (2.0 * np.pi ** 2)) * ppd))


def make_spatial_filters(ppd):
    """1-D CSF kernels: (Y, Cx) as plain Gaussians, Cz as two sqrt-Gaussians.

    Cz's filter is a *sum* of two 2-D Gaussians, which does not separate into a
    single 1-D pass -- hence two half-kernels whose outputs are combined only
    after the vertical pass.
    """
    radius = spatial_filter_radius(ppd)
    width = 2 * radius + 1
    dx = 1.0 / ppd
    ix = (np.arange(width) - radius) * dx
    ix2 = ix * ix

    g_y = _gaussian_ab(ix2, A1[0], B1[0])
    g_cx = _gaussian_ab(ix2, A1[1], B1[1])
    g_cz1 = _gaussian_sqrt_ab(ix2, A1[2], B1[2])
    g_cz2 = _gaussian_sqrt_ab(ix2, A2[2], B2[2])

    g_y = g_y / g_y.sum()
    g_cx = g_cx / g_cx.sum()
    # The two Cz halves share one normaliser, taken over their combined energy.
    norm_cz = 1.0 / np.hypot(g_cz1.sum(), g_cz2.sum())
    return g_y, g_cx, g_cz1 * norm_cz, g_cz2 * norm_cz


def make_feature_filter(ppd):
    """Gaussian plus its first and second derivatives, each separately normalised."""
    std = 0.5 * GW * ppd
    radius = int(np.ceil(3.0 * std))
    width = 2 * radius + 1
    xx = (np.arange(width) - radius).astype(np.float64)

    g = np.exp(-(xx * xx) / (2.0 * std * std))
    dg = -xx * g
    ddg = ((xx * xx) / (std * std) - 1.0) * g

    g = g / g.sum()
    # Positive and negative lobes are normalised independently, so the
    # derivative kernels sum to +1 and -1 rather than to zero.
    dg = np.where(dg > 0.0, dg / dg[dg > 0.0].sum(), dg / -dg[dg <= 0.0].sum())
    ddg = np.where(ddg > 0.0, ddg / ddg[ddg > 0.0].sum(), ddg / -ddg[ddg <= 0.0].sum())
    return g, dg, ddg


# ---------------------------------------------------------------------------
# The two error terms
# ---------------------------------------------------------------------------

def _color_difference(ref_ycxcz, test_ycxcz, ppd):
    g_y, g_cx, g_cz1, g_cz2 = make_spatial_filters(ppd)
    cmax = _max_distance()
    pccmax = GPC * cmax

    def filtered(img):
        y, cx, cz = img[..., 0], img[..., 1], img[..., 2]
        # Horizontal pass, then vertical, per channel.
        y = _convolve1d(_convolve1d(y, g_y, 1), g_y, 0)
        cx = _convolve1d(_convolve1d(cx, g_cx, 1), g_cx, 0)
        cz = (_convolve1d(_convolve1d(cz, g_cz1, 1), g_cz1, 0)
              + _convolve1d(_convolve1d(cz, g_cz2, 1), g_cz2, 0))
        ycxcz = np.stack([y, cx, cz], axis=-1)
        # Round-trip through clamped linear RGB: the CSF can push values out of
        # gamut, and FLIP judges only what a display could actually show.
        lin = np.clip(xyz_to_linear_rgb(ycxcz_to_xyz(ycxcz)), 0.0, 1.0)
        return _hunt(xyz_to_cielab(linear_rgb_to_xyz(lin)))

    diff = _hyab(filtered(ref_ycxcz), filtered(test_ycxcz)) ** GQC
    return np.where(diff < pccmax,
                    diff * (GPT / pccmax),
                    GPT + ((diff - pccmax) / (cmax - pccmax)) * (1.0 - GPT))


def _feature_difference(ref_ycxcz, test_ycxcz, ppd):
    g, dg, ddg = make_feature_filter(ppd)

    def edge_point(img):
        # FLIP runs its detectors on Y renormalised back to [0, 1].
        y = img[..., 0] / 116.0 + 16.0 / 116.0
        dx = _convolve1d(_convolve1d(y, dg, 1), g, 0)
        ddx = _convolve1d(_convolve1d(y, ddg, 1), g, 0)
        gx = _convolve1d(y, g, 1)
        dy = _convolve1d(gx, dg, 0)
        ddy = _convolve1d(gx, ddg, 0)
        return np.hypot(dx, dy), np.hypot(ddx, ddy)

    edge_r, point_r = edge_point(ref_ycxcz)
    edge_t, point_t = edge_point(test_ycxcz)
    worst = np.maximum(np.abs(edge_r - edge_t), np.abs(point_r - point_t))
    return (worst / np.sqrt(2.0)) ** GQF


# ---------------------------------------------------------------------------
# Pooling (pooling.h: pooling<T>::getPercentile with bWeighted = true)
# ---------------------------------------------------------------------------

def weighted_percentile(err, percent):
    """FLIP's error-weighted percentile over the sorted error values.

    Walk the errors in ascending order accumulating their sum, and return the
    first value at which that running sum passes `percent` of the total. Each
    pixel therefore votes in proportion to how wrong it is, so the statistic
    answers "how bad is the error where the error actually is" -- which is why
    it survives a frame that is 99% correct and the mean does not.

    ⚠️ `pooling.h` carries *two* weighted percentiles: this one, and a
    histogram-bucketed `getWeightedPercentile`. They are not interchangeable --
    on a sparse defect the 100-bucket version reads 0.264 where this one reads
    0.551, because bucketing smears a few catastrophic pixels across a decade of
    error. The reference tool reports *this* one, so this is the one that makes
    our numbers comparable to published FLIP scores.
    """
    v = np.sort(err.ravel())
    total = v.sum()
    if total <= 0.0:
        return 0.0
    # `> percent * total`, strictly, matching the reference's loop condition.
    idx = int(np.searchsorted(np.cumsum(v), percent * total, side="right"))
    return float(v[min(idx, v.size - 1)])


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def flip(reference, test, ppd=HANDHELD_PPD):
    """Compute the LDR-FLIP error map and its pooled statistics.

    `reference` and `test` are HxWx3 arrays: uint8 in [0, 255], or float in
    [0, 1]. Returns `(error_map, stats)` with the map in [0, 1].
    """
    ref = np.asarray(reference, dtype=np.float64)
    tst = np.asarray(test, dtype=np.float64)
    if ref.shape != tst.shape:
        raise ValueError(f"shape mismatch: {ref.shape} vs {tst.shape}")
    if ref.ndim != 3 or ref.shape[2] != 3:
        raise ValueError(f"expected HxWx3, got {ref.shape}")
    if np.asarray(reference).dtype == np.uint8:
        ref = ref / 255.0
    if np.asarray(test).dtype == np.uint8:
        tst = tst / 255.0

    ref_ycxcz = xyz_to_ycxcz(linear_rgb_to_xyz(srgb_to_linear(ref)))
    test_ycxcz = xyz_to_ycxcz(linear_rgb_to_xyz(srgb_to_linear(tst)))

    color = _color_difference(ref_ycxcz, test_ycxcz, ppd)
    feature = _feature_difference(ref_ycxcz, test_ycxcz, ppd)
    err = color ** (1.0 - feature)

    return err, pooled_stats(err, ppd)


def pooled_stats(err, ppd=None):
    stats = {
        "mean": float(err.mean()),
        "weighted_median": weighted_percentile(err, 0.5),
        "weighted_q1": weighted_percentile(err, 0.25),
        "weighted_q3": weighted_percentile(err, 0.75),
        "min": float(err.min()),
        "max": float(err.max()),
    }
    if ppd is not None:
        stats["ppd"] = round(float(ppd), 4)
    return stats
