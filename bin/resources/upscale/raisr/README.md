# RAISR texture upscaler filters

These are the trained 2x filter sets used by the RAISR texture upscaler
(`pcsx2/GS/Renderers/HW/GSTextureUpscaler.cpp`).

Source: Intel Library for Video Super Resolution,
https://github.com/OpenVisualCloud/Video-Super-Resolution-Library, commit `ea37e77`.
The files are copied unmodified under Intel's BSD-3-Clause licence (`LICENSE`, next to this file).

## Layout

Each directory holds the 8-bit, single-pass 2x set: `filterbin_2_8` (the filter taps),
`Qfactor_strbin_2_8` and `Qfactor_cohbin_2_8` (the strength and coherence bucket edges).

| Directory | Upstream path | Trained on |
|---|---|---|
| `sharp/`  | `filters_2x/filters_highres/` | 675 pexels images, with area, bicubic, Lanczos, nearest and blur degradations. The 8-bit single-pass filter uses the multi-degradation low-resolution images, with a sharpened (s=1.5) high-resolution image as the reference. |
| `smooth/` | `filters_2x/filters_lowres/`  | 5000 coco_val images, with bicubic degradation only. |

Both use 24 angles x 3 strengths x 3 coherences, 4 pixel phases and 11x11 taps. The cheap
upscale they were trained against is bilinear.

## File format

`filterbin_2_8`: a 16-byte header (`"fp32"`, then little-endian u32 hash count 216, pixel phases 4,
taps 121), then 216 x 4 x 121 little-endian float32 values, hash-major then phase.
The `Qfactor_*` files hold two whitespace-separated decimal edges each.
