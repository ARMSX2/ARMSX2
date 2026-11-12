// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team, 2026 ARMSX2 contributors
// SPDX-License-Identifier: GPL-3.0+

// The run program (GSRunsVK). A run is a stretch of consecutive draws of one render pass that the classic path would
// have drawn under different programs, textures and constants but under one fixed-function state (blend, depth test
// and write, colour mask). The run is one host draw. Each draw keeps its own row in a table: the vertex carries the
// draw's row number in the high bytes of its FOG word, the vertex program hands the row to the fragment program as
// two flat values, and the fragment program does what that draw's classic program (tfx.glsl) does, sampling the draw's
// own texture through its own sampler from the run's descriptor array.
//
// The arithmetic is tfx.glsl's for the state set the machine accepts: FST 0, a 32-bit colour target, no palette in the
// shader, the hardware's bilinear (LTF 0), CLAMP or REPEAT (the sampler's), alpha fail KEEP, the texture functions 0-4,
// and the blend family (Cs - 0) * C + 0 in the shader with the rest in the blend unit (BLEND_MIX). How the classic path
// chose to blend is one of six ways under the run's one dual-source pipeline (ONE, INV_SRC1_COLOR / ONE, ZERO):
//   0  the shader blend, C = As, second output As
//   1  the shader blend, C = Af (the blend constant, or the second output where the driver ignores the constant)
//   2  blending elided, every channel written                (second output 1: the target's term is 0)
//   3  blending elided, alpha written alone                  (colour 0 and second output 0: RGB kept)
//   4  additive, Cs * As in the shader and the target added in the blend unit (second output 0)
//   5  additive, Cs * Af likewise
//
// Specialisation: each per-draw state the run holds one value of arrives as a compile-time constant RUN_C_<state>; only
// the states that vary inside the run are read from the row.

layout(std140, set = 0, binding = 0) uniform RunHeader
{
	vec2 VertexScale;
	vec2 VertexOffset;
	vec2 TextureOffset;
	vec2 STScale;
	uint MaxDepth;
	uint run_pad0;
	uint run_pad1;
	uint run_pad2;
};

#if defined(VERTEX_SHADER)

layout(std430, set = 0, binding = 1) readonly buffer RunTable
{
	uvec4 run_table[];
};

layout(location = 0) in vec2 a_st;
layout(location = 1) in uvec4 a_c;
layout(location = 2) in float a_q;
layout(location = 3) in uvec2 a_p;
layout(location = 4) in uint a_z;
layout(location = 5) in uvec2 a_uv;
layout(location = 6) in vec4 a_f;
layout(location = 7) in uint a_fword; // the same FOG word: fog in its low byte, the draw's row above it

layout(location = 0) out VSOutput
{
	vec4 t;
	vec4 c;
	flat vec4 cf;
	flat uvec4 s0;
	flat vec4 s1;
} vsOut;

void main()
{
	uint z = min(a_z, MaxDepth);
	gl_Position = vec4(a_p, float(z), 1.0f) - vec4(0.05f, 0.05f, 0, 0);
	gl_Position.xy = gl_Position.xy * vec2(VertexScale.x, -VertexScale.y) - vec2(VertexOffset.x, -VertexOffset.y);
	gl_Position.z *= exp2(-32.0f);
	gl_Position.y = -gl_Position.y;
#if GPU_PROFILE_MALI
	if (gl_Position.z == gl_Position.w) gl_Position.z *= 0.999999f; // tfx.glsl's
#endif

	vsOut.t.xy = a_st - TextureOffset;
	vsOut.t.w = a_q;
	vsOut.t.z = a_f.r;
	vsOut.c = vec4(a_c);
	vsOut.cf = vec4(a_c);

	uint row = (a_fword >> 8) * 2u;
	vsOut.s0 = run_table[row];
	vsOut.s1 = uintBitsToFloat(run_table[row + 1u]);
}

#endif

#if defined(FRAGMENT_SHADER)

layout(set = 1, binding = 0) uniform sampler2D RunTextures[];

layout(location = 0) in VSOutput
{
	vec4 t;
	vec4 c;
	flat vec4 cf;
	flat uvec4 s0;
	flat vec4 s1;
} vsIn;

#if RUN_DUAL
layout(location = 0, index = 0) out vec4 o_col0;
layout(location = 0, index = 1) out vec4 o_col1;
#else
layout(location = 0) out vec4 o_col0;
#endif

// s0.x, the draw's flags (GSRunsPolicy.h): 0-2 tfx, 3 tcc, 4 fog, 5 iip, 6-8 alpha test, 9-11 blend way,
//   12-13 lod (0 none, 1 manual, 2 the hardware's), 14 RTA source correction
// s0.y: the draw's slot in the run's texture array
// s0.z: fog colour in bytes 0-2, Af * 128 in byte 3
// s0.w: AREF (float bits)
// s1: the manual lod's K, L, bias, max lod (LODParams)
uint fld(uint v, int s, int n)
{
	return bitfieldExtract(v, s, n);
}

#ifdef RUN_C_TFX
#define S_TFX uint(RUN_C_TFX)
#else
#define S_TFX fld(f, 0, 3)
#endif
#ifdef RUN_C_TCC
#define S_TCC uint(RUN_C_TCC)
#else
#define S_TCC fld(f, 3, 1)
#endif
#ifdef RUN_C_FOG
#define S_FOG uint(RUN_C_FOG)
#else
#define S_FOG fld(f, 4, 1)
#endif
#ifdef RUN_C_IIP
#define S_IIP uint(RUN_C_IIP)
#else
#define S_IIP fld(f, 5, 1)
#endif
#ifdef RUN_C_ATST
#define S_ATST uint(RUN_C_ATST)
#else
#define S_ATST fld(f, 6, 3)
#endif
#ifdef RUN_C_WAY
#define S_WAY uint(RUN_C_WAY)
#else
#define S_WAY fld(f, 9, 3)
#endif
#ifdef RUN_C_LOD
#define S_LOD uint(RUN_C_LOD)
#else
#define S_LOD fld(f, 12, 2)
#endif
#ifdef RUN_C_RTASRC
#define S_RTASRC uint(RUN_C_RTASRC)
#else
#define S_RTASRC fld(f, 14, 1)
#endif

void main()
{
	uvec4 s0 = vsIn.s0;
	vec4 s1 = vsIn.s1;
	uint f = s0.x;
	uint tfx = S_TFX;

	// ps_color(), FST 0, ADJS/ADJT 0: sample_c's uv
	vec2 st = vsIn.t.xy / vsIn.t.w;
	vec2 uv = st * STScale;

	vec4 T = vec4(0.0f);
	if (tfx != 4u)
	{
		uint ti = s0.y;
		uint lod_mode = S_LOD;
		vec4 c;
		// The same three calls tfx.glsl's sample_c makes, on the draw's own texture and sampler. The row's values are
		// the same across a quad (a quad never spans two primitives), so the hardware lod's implicit derivatives are
		// the classic draw's even where the call sits under a branch on them.
		if (lod_mode == 2u)
			c = texture(RunTextures[nonuniformEXT(ti)], uv);
		else if (lod_mode == 1u)
			c = textureLod(RunTextures[nonuniformEXT(ti)], uv, min(s1.x - log2(abs(vsIn.t.w)) * s1.y, s1.w) - s1.z);
		else
			c = textureLod(RunTextures[nonuniformEXT(ti)], uv, 0);
		if (S_RTASRC != 0u)
			c.a = c.a * (128.5f / 255.0f); // PS_RTA_SRC_CORRECTION
		T = trunc(c * 255.0f + 0.05f);
	}

	// tfx()
	vec4 Cv = (S_IIP != 0u) ? vsIn.c : vsIn.cf;
	vec4 FxT = trunc((Cv * T) / 128.0f);
	vec4 C;
	if (tfx == 0u)
		C = FxT;
	else if (tfx == 1u)
		C = T;
	else if (tfx == 2u)
		C = vec4(FxT.rgb + Cv.a, T.a + Cv.a);
	else if (tfx == 3u)
		C = vec4(FxT.rgb + Cv.a, T.a);
	else
		C = Cv;
	if (S_TCC == 0u)
		C.a = Cv.a;
	if (tfx == 0u || tfx == 2u || tfx == 3u)
		C = min(C, 255.0f);

	// fog()
	if (S_FOG != 0u)
	{
		vec3 fog_color = vec3(float(fld(s0.z, 0, 8)), float(fld(s0.z, 8, 8)), float(fld(s0.z, 16, 8)));
		C.rgb = trunc(mix(fog_color, C.rgb, (vsIn.t.z * 255.0f) / 256.0f));
	}

	// atst(), alpha fail KEEP
	uint atst = S_ATST;
	if (atst != 0u)
	{
		float aref = uintBitsToFloat(s0.w);
		bool pass = true;
		if (atst == 1u)
			pass = (C.a <= aref);
		else if (atst == 2u)
			pass = (C.a >= aref);
		else if (atst == 3u)
			pass = (abs(C.a - aref) <= 0.5f);
		else if (atst == 4u)
			pass = (abs(C.a - aref) >= 0.5f);
		if (!pass)
			discard;
	}

	vec4 alpha_blend = vec4(C.a / 128.0f);
	uint way = S_WAY;

	// ps_blend() for the family (A = Cs, B = 0, D = 0, BLEND_MIX), then ps_color_clamp_wrap()
	if (way <= 1u)
	{
		float Cb = (way == 0u) ? alpha_blend.a : (float(fld(s0.z, 24, 8)) / 128.0f);
		C.rgb = (C.rgb * min(Cb, 1.0f)) - (124.0f / 256.0f);
		C.rgb = clamp(C.rgb, vec3(0.0f), vec3(255.0f));
	}
	else if (way >= 4u)
	{
		// BLEND_MIX 0: C unclamped, the product truncated (tfx.glsl's trunc((A - B) * C + D))
		float Cb = (way == 4u) ? alpha_blend.a : (float(fld(s0.z, 24, 8)) / 128.0f);
		C.rgb = clamp(trunc(C.rgb * Cb), vec3(0.0f), vec3(255.0f));
	}

#if RUN_RTA_CORRECTION
	o_col0.a = C.a / 128.0f;
#else
	o_col0.a = C.a / 255.0f;
#endif
	o_col0.rgb = C.rgb / 255.0f;
#if RUN_ZFLOOR
	// PS_ZFLOOR: the depth floored to the GS's integer grid before the test, written from here.
	gl_FragDepth = floor(gl_FragCoord.z * exp2(32.0f)) * exp2(-32.0f);
#endif
#if RUN_DUAL
	if (way == 0u)
		o_col1 = alpha_blend;
	else if (way == 1u)
		o_col1 = vec4(float(fld(s0.z, 24, 8)) / 128.0f);
	else if (way == 2u)
		o_col1 = vec4(1.0f);
	else if (way == 3u)
	{
		o_col0.rgb = vec3(0.0f);
		o_col1 = vec4(0.0f);
	}
	else
		o_col1 = vec4(0.0f);
#endif
}

#endif
