// Snapdragon Game Super Resolution 1.0, "mobile" variant.
//
// SPDX-FileCopyrightText: Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
// The filter body below is Qualcomm's, unchanged in substance. What differs from their sample is
// the shape around it: theirs is a fragment shader over a fullscreen triangle, and this is a
// compute pass, because that is what GSDevice already knows how to schedule (see fsr1.glsl and
// DoFSR1Pass). So the interpolated texcoord becomes an explicit UV computed from the invocation
// id, and the fragment output becomes an imageStore.
//
// The crop handling comes from the Eden Emulator Project's port (GPL-3.0-or-later), which found
// that the source rect has to be mapped explicitly rather than assumed to be the whole texture --
// PCSX2 hands us a display rectangle inside a larger target for exactly the same reason FSR1's
// FsrEasuConOffset takes an offset. The sharpness range being 0..2 rather than 0..1 comes from
// there too; the original was too tight to be useful at the top end.
//
// Brought to ARMSX2 at the suggestion of CamilleLaVey, who authored the Eden changes this is
// based on (eden-emu/eden PR #4293).

#define EDGE_THRESHOLD (8.0 / 255.0)

layout(push_constant) uniform const_buffer
{
    // Output extent, for the bounds check. A dispatch is rounded up to whole workgroups, so the
    // last one runs partly outside the image.
    uvec2 dstSize;
    // The displayed region inside the source texture, normalised. PCSX2's merge target is bigger
    // than the picture in it; without this the filter would upscale the padding too.
    vec2 uvOffset;
    vec2 uvScale;
    // Source texture dimensions and their reciprocal. Qualcomm's "size" and "scale".
    vec2 srcSize;
    vec2 invSrcSize;
    // 0..2. 1.0 is Qualcomm's own default; above that is oversharpened and is offered because
    // the range was too tight to be useful at the top end.
    float edgeSharpness;
};

layout(set = 0, binding = 0) uniform sampler2D imgSrc;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D imgDst;

vec4 weightY(vec4 dx, vec4 dy, vec4 std)
{
    vec4 x = ((dx * dx) + (dy * dy)) * 0.55f + std;
    return (x - 1.f) * (x - 4.f) * 3.8125f; // approx. of (x - 1) * (x - 4)^3
}

layout(local_size_x = 8, local_size_y = 8) in;
void main()
{
    const uvec2 pos = gl_GlobalInvocationID.xy;
    if (pos.x >= dstSize.x || pos.y >= dstSize.y)
        return;

    // Centre of this output pixel, mapped into the displayed region of the source.
    const vec2 texcoord = uvOffset + ((vec2(pos) + vec2(0.5f)) / vec2(dstSize)) * uvScale;

    vec4 color = textureLod(imgSrc, texcoord, 0.0f);

    // image coord
    vec2 icoord = (texcoord * srcSize + vec2(-0.5f, 0.5f));
    vec2 icoord_pixel = floor(icoord);
    vec2 coord = icoord_pixel * invSrcSize;
    vec2 pl = icoord - icoord_pixel;
    // left: 0, right: 1, upDown: 2
    mat3x4 dg = mat3x4(
        textureGather(imgSrc, coord, 1),
        textureGather(imgSrc, coord + vec2(2.f * invSrcSize.x, 0.0f), 1),
        vec4(
            textureGather(imgSrc, coord + vec2(invSrcSize.x, -invSrcSize.y), 1).wz,
            textureGather(imgSrc, coord + vec2(invSrcSize.x, +invSrcSize.y), 1).yx
        )
    );
    float edgeVote = abs(dg[0].z - dg[0].y) + abs(color.y - dg[0].y) + abs(color.y - dg[0].z);
    if (edgeVote > EDGE_THRESHOLD)
    {
        float mean = (dg[0].y + dg[0].z + dg[1].x + dg[1].w) * 0.25f;
        dg = dg - mean;
        vec4 sum = abs(dg[0]) + abs(dg[1]) + abs(dg[2]);
        float std = 2.181818f / (sum.x + sum.y + sum.z + sum.w);
        mat2x4 w = mat2x4(
            weightY(
                pl.xxxx + vec4(+1.0f, +0.0f, +0.0f, +1.0f),
                pl.yyyy + vec4(-1.0f, -1.0f, +0.0f, +0.0f),
                clamp(abs(dg[0]) * std, 0.0f, 1.0f)
            ) + weightY(
                pl.xxxx + vec4(-1.0f, -2.0f, -2.0f, -1.0f),
                pl.yyyy + vec4(-1.0f, -1.0f, +0.0f, +0.0f),
                clamp(abs(dg[1]) * std, 0.0f, 1.0f)
            ) + weightY(
                pl.xxxx + vec4(+0.0f, -1.0f, -1.0f, +0.0f),
                pl.yyyy + vec4(+1.0f, +1.0f, -2.0f, -2.0f),
                clamp(abs(dg[2]) * std, 0.0f, 1.0f)
            ),
            dg[0] + dg[1] + dg[2]
        );
        // compute final y with bounds
        vec2 yb = vec2(
            min(min(dg[0].y, dg[0].z), min(dg[1].x, dg[1].w)), // min
            max(max(dg[0].y, dg[0].z), max(dg[1].x, dg[1].w))  // max
        );
        vec2 fvy = vec2(
            w[0].x + w[0].y + w[0].z + w[0].w,
            w[1].x + w[1].y + w[1].z + w[1].w
        );
        float fy = clamp((fvy.y / fvy.x) * edgeSharpness, yb[0], yb[1]);
        // Smooth high contrast input
        float dy = clamp(fy - color.y + mean, -23.0f / 255.0f, 23.0f / 255.0f);
        color = clamp(color + dy, 0.0f, 1.0f);
    }
    color.w = 1.0f; // assume alpha channel is not used
    imageStore(imgDst, ivec2(pos), color);
}
