#version 450

/*
    CRT Shader by EasyMode
    License: GPL
*/

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OutputSize;
	uint FrameCount;
	float SHARPNESS_IMAGE;
	float SHARPNESS_EDGES;
	float GLOW_WIDTH;
	float GLOW_HEIGHT;
	float GLOW_HALATION;
	float GLOW_DIFFUSION;
	float MASK_COLORS;
	float MASK_STRENGTH;
	float MASK_SIZE;
	float SCANLINE_SIZE_MIN;
	float SCANLINE_SIZE_MAX;
   float SCANLINE_SHAPE;
   float SCANLINE_OFFSET;
	float GAMMA_INPUT;
	float GAMMA_OUTPUT;
	float BRIGHTNESS;
} params;

#pragma parameter SHARPNESS_IMAGE "Sharpness Image" 1.0 1.0 5.0 1.0
#pragma parameter SHARPNESS_EDGES "Sharpness Edges" 3.0 1.0 5.0 1.0
#pragma parameter GLOW_WIDTH "Glow Width" 0.5 0.05 0.65 0.05
#pragma parameter GLOW_HEIGHT "Glow Height" 0.5 0.05 0.65 0.05
#pragma parameter GLOW_HALATION "Glow Halation" 0.1 0.0 1.0 0.01
#pragma parameter GLOW_DIFFUSION "Glow Diffusion" 0.05 0.0 1.0 0.01
#pragma parameter MASK_COLORS "Mask Colors" 2.0 2.0 3.0 1.0
#pragma parameter MASK_STRENGTH "Mask Strength" 0.3 0.0 1.0 0.05
#pragma parameter MASK_SIZE "Mask Size" 1.0 1.0 9.0 1.0
#pragma parameter SCANLINE_SIZE_MIN "Scanline Size Min." 0.5 0.5 1.5 0.05
#pragma parameter SCANLINE_SIZE_MAX "Scanline Size Max." 1.5 0.5 1.5 0.05
#pragma parameter SCANLINE_SHAPE "Scanline Shape" 2.5 1.0 100.0 0.1
#pragma parameter SCANLINE_OFFSET "Scanline Offset" 1.0 0.0 1.0 1.0
#pragma parameter GAMMA_INPUT "Gamma Input" 2.4 1.0 5.0 0.1
#pragma parameter GAMMA_OUTPUT "Gamma Output" 2.4 1.0 5.0 0.1
#pragma parameter BRIGHTNESS "Brightness" 1.5 0.0 2.0 0.05

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
} global;

#define FIX(c) max(abs(c), 1e-5)
#define PI 3.141592653589
#define TEX2D(c) pow(texture(tex, c).rgb, vec3(params.GAMMA_INPUT))
#define saturate(c) clamp(c, 0.0, 1.0)

mat3x3 get_color_matrix(sampler2D tex, vec2 co, vec2 dx)
{
    return mat3x3(TEX2D(co - dx), TEX2D(co), TEX2D(co + dx));
}

vec3 blur(mat3 m, float dist, float rad)
{
    vec3 x = vec3(dist - 1.0, dist, dist + 1.0) / rad;
    vec3 w = exp2(x * x * -1.0);

    return (m[0] * w.x + m[1] * w.y + m[2] * w.z) / (w.x + w.y + w.z);
}

vec3 filter_gaussian(sampler2D tex, vec2 co, vec2 tex_size)
{
    vec2 dx = vec2(1.0 / tex_size.x, 0.0);
    vec2 dy = vec2(0.0, 1.0 / tex_size.y);
    vec2 pix_co = co * tex_size;
    vec2 tex_co = (floor(pix_co) + 0.5) / tex_size;
    vec2 dist = (fract(pix_co) - 0.5) * -1.0;

    mat3x3 line0 = get_color_matrix(tex, tex_co - dy, dx);
    mat3x3 line1 = get_color_matrix(tex, tex_co, dx);
    mat3x3 line2 = get_color_matrix(tex, tex_co + dy, dx);
    mat3x3 column = mat3x3(blur(line0, dist.x, params.GLOW_WIDTH),
                               blur(line1, dist.x, params.GLOW_WIDTH),
                               blur(line2, dist.x, params.GLOW_WIDTH));

    return blur(column, dist.y, params.GLOW_HEIGHT);
}

vec3 filter_lanczos(sampler2D tex, vec2 co, vec2 tex_size, float sharp)
{
    tex_size.x *= sharp;

    vec2 dx = vec2(1.0 / tex_size.x, 0.0);
    vec2 pix_co = co * tex_size - vec2(0.5, 0.0);
    vec2 tex_co = (floor(pix_co) + vec2(0.5, 0.001)) / tex_size;
    vec2 dist = fract(pix_co);
    vec4 coef = PI * vec4(dist.x + 1.0, dist.x, dist.x - 1.0, dist.x - 2.0);

    coef = FIX(coef);
    coef = 2.0 * sin(coef) * sin(coef / 2.0) / (coef * coef);
    coef /= dot(coef, vec4(1.0));

    vec4 col1 = vec4(TEX2D(tex_co), 1.0);
    vec4 col2 = vec4(TEX2D(tex_co + dx), 1.0);

    return (mat4x4(col1, col1, col2, col2) * coef).rgb;
}

vec3 get_scanline_weight(float x, vec3 col)
{
    vec3 beam = mix(vec3(params.SCANLINE_SIZE_MIN), vec3(params.SCANLINE_SIZE_MAX), pow(col, vec3(1.0 / params.SCANLINE_SHAPE)));
    vec3 x_mul = 2.0 / beam;
    vec3 x_offset = x_mul * 0.5;

    return smoothstep(0.0, 1.0, 1.0 - abs(x * x_mul - x_offset)) * x_offset;
}

vec3 get_mask_weight(float x)
{
    float i = mod(floor(x * params.OutputSize.x * params.SourceSize.x / (params.SourceSize.x * params.MASK_SIZE)), params.MASK_COLORS);

    if (i == 0.0) return mix(vec3(1.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), params.MASK_COLORS - 2.0);
    else if (i == 1.0) return vec3(0.0, 1.0, 0.0);
    else return vec3(0.0, 0.0, 1.0);
}

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
   gl_Position = global.MVP * Position;
   vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
    float scale = floor(params.OutputSize.y * params.SourceSize.w);
    float offset = 1.0 / scale * 0.5;
    
    if (bool(mod(scale, 2.0))) offset = 0.0;
    
    vec2 co = (vTexCoord * params.SourceSize.xy - vec2(0.0, offset * params.SCANLINE_OFFSET)) * params.SourceSize.zw;
    vec3 col_glow = filter_gaussian(Source, co, params.SourceSize.xy);
    vec3 col_soft = filter_lanczos(Source, co, params.SourceSize.xy, params.SHARPNESS_IMAGE);
    vec3 col_sharp = filter_lanczos(Source, co, params.SourceSize.xy, params.SHARPNESS_EDGES);
    vec3 col = sqrt(col_sharp * col_soft);

    col *= get_scanline_weight(fract(co.y * params.SourceSize.y), col_soft);
    col_glow = saturate(col_glow - col);
    col += col_glow * col_glow * params.GLOW_HALATION;
    col = mix(col, col * get_mask_weight(vTexCoord.x) * params.MASK_COLORS, params.MASK_STRENGTH);
    col += col_glow * params.GLOW_DIFFUSION;
    col = pow(col * params.BRIGHTNESS, vec3(1.0 / params.GAMMA_OUTPUT));
   FragColor = vec4(col, 1.0);
}
