#version 450

/*
   Author: rsn8887 (Optimized by community)
   License: Public domain

   Sharp-Bilinear filter for non-integer scaling.
   Warp UVs to get sharp pixel interiors with hardware filtered edges.

   Optimizations:
   - Used SourceSize.zw mult instead of Output/Source div to fix integer scaling artifacts.
   - Moved scale and region calculations to VS.
   - Used 'flat' out to force variables into SGPRs and save VALU cycles.
   - Cleaned up FS math to help compiler generate native FMA and CLAMP.
*/

layout(push_constant) uniform Push
{
  vec4 OutputSize;
  vec4 OriginalSize;
  vec4 SourceSize;
} params;

layout(std140, set = 0, binding = 0) uniform UBO
{
   mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;


layout(location = 0) out vec2 vTexCoord; 
// flat qualifies variables as constant per-primitive, disabling the HW interpolator and forcing the driver to utilize SGPRs to free up VALU budget
layout(location = 1) flat out vec2 precalc_scale;
layout(location = 2) flat out vec2 precalc_region_range; 

void main() {
    gl_Position = global.MVP * Position;
    

    vTexCoord = TexCoord; 
    
    vec2 scale = max(floor(params.OutputSize.xy * params.SourceSize.zw), vec2(1.0));
    precalc_scale = scale;

    // Terminate the expensive division in VS; execution frequency drops from millions per-frame to just 4 times per-vertex
    precalc_region_range = 0.5 - 0.5 / scale; 
}

#pragma stage fragment

layout(location = 0) in vec2 vTexCoord; 
layout(location = 1) flat in vec2 precalc_scale;
layout(location = 2) flat in vec2 precalc_region_range;

layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main() {

    vec2 texel = vTexCoord * params.SourceSize.xy;
    vec2 texel_floored = floor(texel);
    vec2 s = texel - texel_floored; 
    vec2 center_dist = s - 0.5;
    
    // 100% clean semantics matching perfectly with native GPU CLAMP ISA instructions
    vec2 clamped_dist = clamp(center_dist, -precalc_region_range, precalc_region_range);
    
    // Standard FMA structure, automatically folded into a single-cycle hardware instruction by modern GPU compilers
    vec2 f = (center_dist - clamped_dist) * precalc_scale + 0.5; 
    
    vec2 mod_texel = texel_floored + f;
    FragColor = vec4(texture(Source, mod_texel * params.SourceSize.zw).rgb, 1.0);
}
