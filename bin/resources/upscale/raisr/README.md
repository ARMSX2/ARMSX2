# RAISR texture upscaler filters

The filter set used by the RAISR texture upscaler (`pcsx2/GS/Renderers/HW/GSTextureUpscaler.cpp`).
The 2x mode runs it once on a texture, the 4x mode twice.

## ps2/

`filterbin_2_8` (the filter taps), `Qfactor_strbin_2_8` and `Qfactor_cohbin_2_8` (the strength and
coherence bucket edges). These are ARMSX2's own filters, fitted to pairs of PS2 textures and their
community HD replacements. None of Intel's trained weights are in them. Licence: GPL-3.0-or-later,
like the rest of ARMSX2.

The layout is 24 angles x 3 strengths x 3 coherences = 216 hash buckets, 4 pixel phases and 11x11
taps. The cheap upscale they correct is a centre-aligned bilinear 2x of luma. Chroma and alpha are
not filtered.

### How they were trained

- **Pairs.** 1,595 pairs from 18 games, 26.3 million native texels. The input is the texture as the
  game uploads it, palette quantisation and dither included, taken from savestates and GS dumps. The
  target is the matching texture of a community HD pack, box-averaged down to 2x the native size
  (the packs are 4x native, except two games that are partly 2x). A pair is kept only if the pack
  texture, box-shrunk back to native size, agrees with the native texture at 28 dB luma PSNR or
  better over its opaque texels. That drops repainted textures, replaced text and different
  palettes. Flat textures are dropped too.
- **Fit.** Luma only. For each hash bucket and pixel phase, ridge regression of the target on the
  11x11 window of the engine's own bilinear 2x luma, with the ridge pulled toward the pooled kernel
  of that phase (lambda 1e-5 of the mean second moment). Buckets are assigned by the engine's own
  hash code, so the fit sees what the upscaler sees. 8 augmentations (4 rotations, with and without
  a transpose) applied to each image pair before hashing. Each game counts equally. At most 200,000
  pixels per texture and transform.
- **Bucket edges.** The tertiles of strength and of coherence over the training pixels, so each
  strength and each coherence bucket holds a third of them. Intel's edges are not used.
- **Result.** Luma PSNR over opaque texels with each game held out of the fit in turn, mean over
  the 18 games: bilinear 29.74 dB, Intel's Smooth filters 30.80 dB, this set 31.01 dB (+0.20 dB
  over Smooth). Through the full in-game path (range clamp and hard alpha edges) the gain is +0.17
  dB. It is better than Smooth on 16 of 18 games; it is worse on Beyond Good & Evil (-0.08 dB) and
  Xenosaga (-0.05 dB). No setting of the fit moved the result by more than 0.04 dB, so what limits
  it is the 121-tap linear filter, not the data. Detail that an AI upscaler invented in a pack
  texture is out of its reach.
- **Flat areas.** The bucket for flat patches sums to 0.9976, not 1: in the training pairs the
  pack is on average a hair darker than the native texture. The engine on its own can leave 255 at
  254; the upscaler's range clamp puts a flat patch back exactly.

### Texture packs the pairs came from

Pack names are the ids the packs were downloaded under. The author is the one the catalogue gives;
a dagger marks a credit that comes from the archive list the pack was preserved from, not from the
pack itself. The pack textures are not in this tree, only the fitted weights.

| Game | Pack | Author | Pairs |
|---|---|---|---|
| Beyond Good & Evil | `panda-venom-beyond-good-and-evil-test-slus-20763-20260726` | Panda_Venom | 88 |
| Battlefield 2: Modern Combat | `robin9608-battlefield-2-modern-combat-slus-21026-20260728` | RoBin9608 † | 30 |
| Black | `huekage-black-v2-slus-21376-sles-53886-20260926` | Huekage | 40 |
| Burnout 3: Takedown | `panda-venom-burnout-3-takedown-20260722` | Panda_Venom | 89 |
| Dragon Quest VIII: Journey of the Cursed King | `1vierock-dragon-quest-viii-journey-of-the-cursed-king-slus-21207-20260927` | 1vierock | 59 |
| Dynasty Warriors 3 XL | `sombertwilight-dynasty-warriors-3-xl-slus-20277-20260728` | SomberTwilight | 37 |
| God of War II | `panda-venom-god-of-war-ii-20260722` | Panda_Venom | 380 |
| Guitar Hero II | `callofcthuloo-guitar-hero-2-desktop-slus-21447-20260728` | callofcthuloo | 105 |
| Jak II | `unknown-archive-jak-ii-scus-97265-20260728` | not identified | 211 |
| Metal Gear Solid 3: Subsistence | `spider-man-metal-gear-solid-3-subsistence-slus-21359-20260728` | Spider_Man † | 19 |
| Mortal Kombat: Shaolin Monks | `bl4ckh4nd-mortal-kombat-shaolin-monks-slus-21087-20260728` | Bl4ckH4nd | 50 |
| Need for Speed: Underground | `cckrizalid-nfs-underground-slus-20811-v20221016-20260927` | CCKrizalid | 91 |
| Ratchet & Clank: Up Your Arsenal | `texmaster-ratchet-clank-up-your-arsenal-scus-97353-20260728` | TexMaster † | 8 |
| Scarface: The World Is Yours | `c0meg3ts0me-scarface-the-world-is-yours-slus-21111-20260728` | C0meg3ts0me † | 28 |
| Shadow of the Colossus | `sad-origami-shadow-of-the-colossus-scus-97472-20260728` | Sad Origami † | 109 |
| Sly 3: Honor Among Thieves | `bl4ckh4nd-sly-3-scus-97464-20260722` | Bl4ckH4nd | 233 |
| Xenosaga Episode I | `panda-venom-xenosaga-episode-i-slus-20469-20260728` | Panda_Venom † | 9 |
| Yu-Gi-Oh! The Duelists of the Roses | `unknown-archive-yu-gi-oh-duelist-of-the-roses-slus-20515-20260728` | not identified | 9 |

## File format

`filterbin_2_8`: a 16-byte header (`"fp32"`, then little-endian u32 hash count 216, pixel phases 4,
taps 121), then 216 x 4 x 121 little-endian float32 values, hash-major then phase.
The `Qfactor_*` files hold two whitespace-separated decimal edges each. The format is Intel's.

## Intel's licence

`LICENSE.Intel` is the BSD-3-Clause licence of Intel's Library for Video Super Resolution
(https://github.com/OpenVisualCloud/Video-Super-Resolution-Library, commit `ea37e77`). The upscaler
is a C++ port of the RAISR algorithm as that library implements it, and reads the file format it
defines, so the notice is kept. It does not cover the weights in `ps2/`, which are not derived from
Intel's.
