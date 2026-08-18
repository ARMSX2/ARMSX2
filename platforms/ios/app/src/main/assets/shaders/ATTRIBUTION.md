# Bundled RetroArch shader attribution

ARMSX2 ships a small curated subset of the libretro slang-shaders collection under
`shaders/presets/`. This notice covers every file in that subset: who wrote it, under what
licence, and where it came from.

**Upstream:** `https://github.com/libretro/slang-shaders`
**Pinned commit:** `80372284ea8c00ae5e25e5a6e4f9f49415f85896` (2026-08-15)

Every file below is copied byte for byte from that commit, licence headers intact. Nothing
was renamed, reformatted or edited. To verify any file, clone the upstream repository at the
pinned commit and compare it against the copy in this bundle at the upstream path given.

The upstream collection is mixed licence and has no repository-wide licence file, so each
file here was cleared on its own header rather than on a blanket grant. Files whose headers
state no licence were not bundled.

`shaders/armsx2-tracer/` is not covered by this notice. It is ARMSX2's own shader, written
for this project, and carries the same GPL-3.0-or-later terms as the rest of the application.

---

## CRT

| Bundled path | Upstream path | Author | Licence |
|---|---|---|---|
| `presets/crt/zfast-crt.slangp` | `crt/zfast-crt.slangp` | Greg Hogan (SoltanGris42) | GPL-2.0-or-later |
| `presets/crt/shaders/zfast_crt/zfast_crt_finemask.slang` | `crt/shaders/zfast_crt/zfast_crt_finemask.slang` | Greg Hogan (SoltanGris42) | GPL-2.0-or-later |
| `presets/crt/shaders/zfast_crt/zfast_crt_impl.inc` | `crt/shaders/zfast_crt/zfast_crt_impl.inc` | Copyright (C) 2017 Greg Hogan (SoltanGris42) | GPL-2.0-or-later |
| `presets/crt/crt-hyllian-fast.slangp` | `crt/crt-hyllian-fast.slangp` | Hyllian; ported to GLSL/SLANG by DariusG & hunterk | MIT |
| `presets/crt/shaders/hyllian/crt-hyllian-fast.slang` | `crt/shaders/hyllian/crt-hyllian-fast.slang` | Copyright (C) 2011-2015 Hyllian | MIT |
| `presets/crt/crt-geom.slangp` | `crt/crt-geom.slangp` | cgwg, Themaister and DOLLS | GPL-2.0-or-later |
| `presets/crt/shaders/crt-geom.slang` | `crt/shaders/crt-geom.slang` | Copyright (C) 2010-2012 cgwg, Themaister and DOLLS | GPL-2.0-or-later |
| `presets/crt/crt-aperture.slangp` | `crt/crt-aperture.slangp` | EasyMode | GPL, version unstated |
| `presets/crt/shaders/crt-aperture.slang` | `crt/shaders/crt-aperture.slang` | EasyMode | GPL, version unstated |
| `presets/crt/crt-easymode.slangp` | `crt/crt-easymode.slangp` | EasyMode | GPL, version unstated |
| `presets/crt/shaders/crt-easymode.slang` | `crt/shaders/crt-easymode.slang` | EasyMode | GPL, version unstated |

## Scanlines

| Bundled path | Upstream path | Author | Licence |
|---|---|---|---|
| `presets/scanlines/scanlines-sine-abs.slangp` | `scanlines/scanlines-sine-abs.slangp` | RiskyJumps | Public domain |
| `presets/scanlines/shaders/scanlines-sine-abs.slang` | `scanlines/shaders/scanlines-sine-abs.slang` | RiskyJumps | Public domain |
| `presets/scanlines/res-independent-scanlines.slangp` | `scanlines/res-independent-scanlines.slangp` | RiskyJumps | Public domain |
| `presets/scanlines/shaders/res-independent-scanlines.slang` | `scanlines/shaders/res-independent-scanlines.slang` | RiskyJumps | Public domain |
| `presets/include/subpixel_masks.h` | `include/subpixel_masks.h` | hunterk | Public domain |

## Handheld

| Bundled path | Upstream path | Author | Licence |
|---|---|---|---|
| `presets/handheld/lcd3x.slangp` | `handheld/lcd3x.slangp` | Gigaherz | Public domain |
| `presets/handheld/shaders/lcd3x.slang` | `handheld/shaders/lcd3x.slang` | Gigaherz | Public domain |
| `presets/handheld/sameboy-lcd.slangp` | `handheld/sameboy-lcd.slangp` | LIJI32 (SameBoy) | MIT |
| `presets/handheld/shaders/sameboy-lcd.slang` | `handheld/shaders/sameboy-lcd.slang` | Copyright (c) 2015-2016 Lior Halphon | MIT |

## Pixel-art scaling

| Bundled path | Upstream path | Author | Licence |
|---|---|---|---|
| `presets/pixel-art-scaling/sharp-bilinear.slangp` | `pixel-art-scaling/sharp-bilinear.slangp` | Themaister | Public domain |
| `presets/pixel-art-scaling/shaders/sharp-bilinear.slang` | `pixel-art-scaling/shaders/sharp-bilinear.slang` | Themaister | Public domain |
| `presets/pixel-art-scaling/sharp-bilinear-simple.slangp` | `pixel-art-scaling/sharp-bilinear-simple.slangp` | rsn8887 (optimized by community) | Public domain |
| `presets/pixel-art-scaling/shaders/sharp-bilinear-simple.slang` | `pixel-art-scaling/shaders/sharp-bilinear-simple.slang` | rsn8887 (optimized by community) | Public domain |

---

## Licence notices

**A note on the `.slangp` files.** A `.slangp` is a short `key = value` preset file — a pass
count, a relative path to a shader stage, a filter flag — and none of the bundled ones carries
a licence header of its own. Each is listed above under the licence of the stage it invokes,
which is the only substantive content it has.

**MIT** — `crt-hyllian-fast.slang` and `sameboy-lcd.slang`. Both files carry their copyright
notice and the full MIT permission notice in their own headers, unmodified, as MIT requires.
Read them in the bundled files themselves.

**GPL-2.0-or-later** — `zfast_crt_impl.inc`, `zfast_crt_finemask.slang`, `crt-geom.slang` and
the `.slangp` files that invoke them. Each states it may be redistributed "under the terms of
the GNU General Public License … either version 2 of the License, or (at your option) any
later version", which is what permits it to ship inside a GPL-3.0-or-later application. These
shaders ship as source, so the bundled file is itself the corresponding source.

`crt-geom.slang` records the original author's consent in its header: cgwg wrote "Feel free to
distribute my shaders under the GPL. After all, the barrel distortion code was taken from the
Curvature shader, which is under the GPL."

**GPL with no version stated** — `crt-aperture.slang` and `crt-easymode.slang` by EasyMode
say only "License: GPL". Where a GPL-licensed file names no version, GPL-2.0 section 9 and
GPL-3.0 section 14 permit the recipient to choose any version ever published by the Free
Software Foundation. ARMSX2 takes these under GPL-3.0-or-later on that basis.

**Public domain** — the remaining files place themselves in the public domain in their own
headers, and are reproduced here with their authorship lines intact as a courtesy rather than
as a legal requirement.

## If you redistribute ARMSX2

Keep this file and the `shaders/presets/` tree together, keep every shader's header intact,
and this notice stays accurate. If you add shaders of your own, add them here too — and check
each file's header first, because most of the upstream collection carries no licence grant at
all.
