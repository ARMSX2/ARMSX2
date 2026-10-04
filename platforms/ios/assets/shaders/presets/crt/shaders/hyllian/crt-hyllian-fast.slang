#version 450

/*
   Hyllian's CRT Shader
   with cgwg's magenta/green dotmask
   ported to GLSL/SLANG by DariusG & hunterk
  
   Copyright (C) 2011-2015 Hyllian - sergiogdb@gmail.com

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

layout(push_constant) uniform Push
{
 float MASK_INTENSITY;
 float InputGamma;
 float OutputGamma;
 float BRIGHTBOOST;
 float SCANLINES;
 float SHARPER;
} params;

// Parameter lines go here:


#pragma parameter MASK_INTENSITY "MASK INTENSITY" 0.5 0.0 1.0 0.1
#define MASK_INTENSITY params.MASK_INTENSITY

#pragma parameter InputGamma "INPUT GAMMA" 2.4 0.0 5.0 0.1
#define InputGamma params.InputGamma

#pragma parameter OutputGamma "OUTPUT GAMMA" 2.2 0.0 5.0 0.1
#define OutputGamma params.OutputGamma

#pragma parameter BRIGHTBOOST "BRIGHT BOOST" 1.5 0.0 2.0 0.1
#define BRIGHTBOOST params.BRIGHTBOOST

#pragma parameter SCANLINES "SCANLINES STRENGTH" 0.72 0.0 1.0 0.02
#define SCANLINES params.SCANLINES 

#pragma parameter SHARPER "SHARPER" 0.0 0.0 1.0 1.0
#define SHARPER params.SHARPER 


layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
	vec4 SourceSize;
	vec4 OriginalSize;
	vec4 OutputSize;
	uint FrameCount;
} global;

#define SourceSize global.SourceSize
#define TextureSize SourceSize.xy
#define InputSize global.OriginalSize.xy
#define COMPAT_TEXTURE texture
#define GAMMA_IN(color)     pow(color, vec3(InputGamma, InputGamma, InputGamma))
#define GAMMA_OUT(color)    pow(color, vec3(1.0 / OutputGamma, 1.0 / OutputGamma, 1.0 / OutputGamma))


#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec2 ps;

void main()
{
   gl_Position = global.MVP * Position;
   vec2 tex_size = vec2(TextureSize.x, TextureSize.y);
   ps = 1.0/tex_size;
   vTexCoord.xy = TexCoord.xy + ps * vec2(-0.49999, 0.0);
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec2 ps;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 1) uniform sampler2D Source;


///////////////////////////////////////////////////////////////////////////////////////////////

void main()
{
   vec2 dx = vec2(ps.x, 0.0);
   vec2 dy = vec2(0.0, ps.y);
   
   vec2 tc = (floor(vTexCoord.xy * SourceSize.xy) + vec2(0.49999, 0.49999)) / SourceSize.xy;
   
   vec2 fp = fract(vTexCoord.xy * SourceSize.xy);
   
   vec3 c10 = COMPAT_TEXTURE(Source, tc -       dx).xyz;
   vec3 c11 = COMPAT_TEXTURE(Source, tc           ).xyz;
   vec3 c12 = COMPAT_TEXTURE(Source, tc +       dx).xyz;
   vec3 c13 = COMPAT_TEXTURE(Source, tc + 2.0 * dx).xyz;
	
   vec4 lobes = vec4(fp.x*fp.x*fp.x, fp.x*fp.x, fp.x, 1.0);

   vec4 InvX = vec4(0.0);


   if (SHARPER == 0.0) {
// Horizontal cubic filter - "Catrom"
    InvX.x = dot(vec4( -0.5, 1.0, -0.5, 0.0), lobes);
    InvX.y = dot(vec4(  1.5,-2.5,  0.0, 1.0), lobes);
    InvX.z = dot(vec4( -1.5, 2.0,  0.5, 0.0), lobes);
    InvX.w = dot(vec4(  0.5,-0.5,  0.0, 0.0), lobes);
}

    else if (SHARPER == 1.0) {
// Swith to "Hermite" - Sharper, smoothed bilinear     
    InvX.x = dot(vec4(  0.0, 0.0,  0.0, 0.0), lobes);
    InvX.y = dot(vec4(  2.0,-3.0,  0.0, 1.0), lobes);
    InvX.z = dot(vec4( -2.0, 3.0,  0.0, 0.0), lobes);
    InvX.w = dot(vec4(  0.0, 0.0,  0.0, 0.0), lobes);
}


	
    vec3 color = InvX.x*c10.xyz;
		 color+= InvX.y*c11.xyz;
		 color+= InvX.z*c12.xyz;
		 color+= InvX.w*c13.xyz;
	
	
	color = GAMMA_IN(color);
	
    float pos1 = 1.5-SCANLINES - abs(fp.y - 0.5);
    float d1 = max(0.0, min(1.0, pos1));
    float d = d1*d1*(3.0+BRIGHTBOOST - (2.0*d1));
	
    color = color*d;
    
// dotmask
    float mod_factor = vTexCoord.x * global.OutputSize.x;
    vec4 dotMaskWeights = mix(
                                 vec4(1.0, 1.0-MASK_INTENSITY, 1.0, 1.),
                                 vec4(1.0-MASK_INTENSITY, 1.0, 1.0-MASK_INTENSITY, 1.),
                                 floor(mod(mod_factor, 2.0))
                                  );
    color *=vec3(dotMaskWeights.x,dotMaskWeights.y,dotMaskWeights.z);

    color  = GAMMA_OUT(color);
    FragColor = vec4(color.r, color.g, color.b, 1.0);
} 
