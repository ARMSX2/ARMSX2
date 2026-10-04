#version 450

layout(push_constant) uniform Push
{
	uint FrameCount;
	float CRTgamma;
	float monitorgamma;
	float d;
	float R;
	float cornersize;
	float cornersmooth;
	float x_tilt;
	float y_tilt;
	float overscan_x;
	float overscan_y;
	float mask_type;
	float DOTMASK;
	float DOTMASK_brightboost;
	float SHARPER;
	float scanline_weight;
	float CURVATURE;
   float interlace_detect;
   float lum;
   float invert_aspect;
   float vertical_scanlines;
   float xsize;
   float ysize;
} registers;

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
	vec4 OutputSize;
   vec4 SourceSize;
} global;

#pragma parameter CRTgamma "CRTGeom Target Gamma" 2.4 0.1 5.0 0.1
#pragma parameter monitorgamma "CRTGeom Monitor Gamma" 2.2 0.1 5.0 0.1
#pragma parameter d "CRTGeom Distance" 1.5 0.1 3.0 0.1
#pragma parameter CURVATURE "CRTGeom Curvature Toggle" 1.0 0.0 1.0 1.0
#pragma parameter invert_aspect "CRTGeom Curvature Aspect Inversion" 0.0 0.0 1.0 1.0
#pragma parameter R "CRTGeom Curvature Radius" 2.0 0.1 10.0 0.1
#pragma parameter cornersize "CRTGeom Corner Size" 0.03 0.001 1.0 0.005
#pragma parameter cornersmooth "CRTGeom Corner Smoothness" 1000.0 80.0 2000.0 100.0
#pragma parameter x_tilt "CRTGeom Horizontal Tilt" 0.0 -0.5 0.5 0.05
#pragma parameter y_tilt "CRTGeom Vertical Tilt" 0.0 -0.5 0.5 0.05
#pragma parameter overscan_x "CRTGeom Horiz. Overscan %" 100.0 -125.0 125.0 0.5
#pragma parameter overscan_y "CRTGeom Vert. Overscan %" 100.0 -125.0 125.0 0.5
#pragma parameter mask_type "CRTGeom Mask Pattern" 1.0 1.0 20.0 1.0
#pragma parameter DOTMASK "CRTGeom Mask strength" 0.3 0.0 1.0 0.05
#pragma parameter DOTMASK_brightboost "CRTGeom Mask brightness boost" 0.0 0.0 1.0 0.05
#pragma parameter SHARPER "CRTGeom Sharpness" 1.0 1.0 3.0 1.0
#pragma parameter scanline_weight "CRTGeom Scanline Weight" 0.3 0.1 0.5 0.05
#pragma parameter vertical_scanlines "CRTGeom Vertical Scanlines" 0.0 0.0 1.0 1.0
#pragma parameter lum "CRTGeom Luminance" 0.0 0.0 1.0 0.01
#pragma parameter interlace_detect "CRTGeom Interlacing Simulation" 1.0 0.0 1.0 1.0

#pragma parameter xsize "Simulated Width (0==Auto)" 0.0 0.0 1920.0 16.0
#pragma parameter ysize "Simulated Height (0==Auto)" 0.0 0.0 1080.0 16.0

vec2 height = (registers.ysize > 0.001) ? vec2(registers.ysize, 1./registers.ysize) : global.SourceSize.yw;
vec2 width = (registers.xsize > 0.001) ? vec2(registers.xsize, 1./registers.xsize) : global.SourceSize.xz;
vec4 SourceSize = vec4(width.x, height.x, width.y, height.y);

/*
    CRT-interlaced

    Copyright (C) 2010-2012 cgwg, Themaister and DOLLS

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    (cgwg gave their consent to have the original version of this shader
    distributed under the GPL in this message:

    http://board.byuu.org/viewtopic.php?p=26075#p26075

    "Feel free to distribute my shaders under the GPL. After all, the
    barrel distortion code was taken from the Curvature shader, which is
    under the GPL."
    )
    This shader variant is pre-configured with screen curvature
*/

// Comment the next line to disable interpolation in linear gamma (and
// gain speed).
#define LINEAR_PROCESSING

// Enable 3x oversampling of the beam profile; improves moire effect caused by scanlines+curvature
#define OVERSAMPLE

// Use the older, purely gaussian beam profile; uncomment for speed
//#define USEGAUSSIAN

// Macros.
#define FIX(c) max(abs(c), 1e-5);
#define PI 3.141592653589

#ifdef LINEAR_PROCESSING
#       define TEX2D(c) pow(texture(Source, (c)), vec4(registers.CRTgamma))
#else
#       define TEX2D(c) texture(Source, (c))
#endif

#define u_quad_dims global.OutputSize.xy
#define u_tex_size1 vec2(global.OutputSize.xy * global.SourceSize.zw)

#define DOTMASK registers.DOTMASK
#define DOTMASK_brightboost registers.DOTMASK_brightboost

// aspect ratio
vec2 aspect     = vec2(registers.invert_aspect > 0.5 ? (0.75, 1.0) : (1.0, 0.75));
vec2 overscan   = vec2(1.01, 1.01);

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec2 sinangle;
layout(location = 2) out vec2 cosangle;
layout(location = 3) out vec3 stretch;
layout(location = 4) out vec2 ilfac;
layout(location = 5) out vec2 one;
layout(location = 6) out float mod_factor;
layout(location = 7) out vec2 TextureSize;

float intersect(vec2 xy)
{
    float A = dot(xy,xy) + registers.d*registers.d;
    float B = 2.0*(registers.R*(dot(xy,sinangle)-registers.d*cosangle.x*cosangle.y)-registers.d*registers.d);
    float C = registers.d*registers.d + 2.0*registers.R*registers.d*cosangle.x*cosangle.y;
    
    return (-B-sqrt(B*B-4.0*A*C))/(2.0*A);
}

vec2 bkwtrans(vec2 xy)
{
    float c     = intersect(xy);
    vec2 point  = (vec2(c, c)*xy - vec2(-registers.R, -registers.R)*sinangle) / vec2(registers.R, registers.R);
    vec2 poc    = point/cosangle;
    
    vec2 tang   = sinangle/cosangle;
    float A     = dot(tang, tang) + 1.0;
    float B     = -2.0*dot(poc, tang);
    float C     = dot(poc, poc) - 1.0;
    
    float a     = (-B + sqrt(B*B - 4.0*A*C))/(2.0*A);
    vec2 uv     = (point - a*sinangle)/cosangle;
    float r     = FIX(registers.R*acos(a));
    
    return uv*r/sin(r/registers.R);
}

vec2 fwtrans(vec2 uv)
{
    float r = FIX(sqrt(dot(uv,uv)));
    uv *= sin(r/registers.R)/r;
    float x = 1.0-cos(r/registers.R);
    float D = registers.d/registers.R + x*cosangle.x*cosangle.y+dot(uv,sinangle);
    
    return registers.d*(uv*cosangle-x*sinangle)/D;
}

vec3 maxscale()
{
    vec2 c  = bkwtrans(-registers.R * sinangle / (1.0 + registers.R/registers.d*cosangle.x*cosangle.y));
    vec2 a  = vec2(0.5,0.5)*aspect;
    
    vec2 lo = vec2(fwtrans(vec2(-a.x,  c.y)).x,
                   fwtrans(vec2( c.x, -a.y)).y)/aspect;

    vec2 hi = vec2(fwtrans(vec2(+a.x,  c.y)).x,
                   fwtrans(vec2( c.x, +a.y)).y)/aspect;
    
    return vec3((hi+lo)*aspect*0.5,max(hi.x-lo.x,hi.y-lo.y));
}

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord * vec2(1.00001);

    // Precalculate a bunch of useful values we'll need in the fragment
    // shader.
    sinangle    = sin(vec2(registers.x_tilt, registers.y_tilt));
    cosangle    = cos(vec2(registers.x_tilt, registers.y_tilt));
    stretch     = maxscale();
    
    if(registers.vertical_scanlines < 0.5)
    {
       TextureSize = vec2(registers.SHARPER * SourceSize.x, SourceSize.y);
       
       ilfac = vec2(1.0, clamp(floor(SourceSize.y/(registers.interlace_detect > 0.5 ? 200.0 : 1000)),  1.0, 2.0));

       // The size of one texel, in texture-coordinates.
       one = ilfac / TextureSize;

       // Resulting X pixel-coordinate of the pixel we're drawing.
       mod_factor = vTexCoord.x * SourceSize.x * global.OutputSize.x / SourceSize.x;
    }else{
       TextureSize = vec2(SourceSize.x, registers.SHARPER * SourceSize.y);

       ilfac = vec2(clamp(floor(SourceSize.x/(registers.interlace_detect > 0.5 ? 200.0 : 1000)),  1.0, 2.0), 1.0);

       // The size of one texel, in texture-coordinates.
       one = ilfac / TextureSize;

       // Resulting X pixel-coordinate of the pixel we're drawing.
       mod_factor = vTexCoord.y * SourceSize.y * global.OutputSize.y / SourceSize.y;
    }
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec2 sinangle;
layout(location = 2) in vec2 cosangle;
layout(location = 3) in vec3 stretch;
layout(location = 4) in vec2 ilfac;
layout(location = 5) in vec2 one;
layout(location = 6) in float mod_factor;
layout(location = 7) in vec2 TextureSize;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

float intersect(vec2 xy)
{
    float A = dot(xy,xy) + registers.d*registers.d;
    float B, C;

    if(registers.vertical_scanlines < 0.5)
    {
       B = 2.0*(registers.R*(dot(xy,sinangle) - registers.d*cosangle.x*cosangle.y) - registers.d*registers.d);
       C = registers.d*registers.d + 2.0*registers.R*registers.d*cosangle.x*cosangle.y;
    }else{
       B = 2.0*(registers.R*(dot(xy,sinangle) - registers.d*cosangle.y*cosangle.x) - registers.d*registers.d);
       C = registers.d*registers.d + 2.0*registers.R*registers.d*cosangle.y*cosangle.x;
    }

    return (-B-sqrt(B*B - 4.0*A*C))/(2.0*A);
}

vec2 bkwtrans(vec2 xy)
{
    float c     = intersect(xy);
    vec2 point  = (vec2(c, c)*xy - vec2(-registers.R, -registers.R)*sinangle) / vec2(registers.R, registers.R);
    vec2 poc    = point/cosangle;
    vec2 tang   = sinangle/cosangle;

    float A     = dot(tang, tang) + 1.0;
    float B     = -2.0*dot(poc, tang);
    float C     = dot(poc, poc) - 1.0;

    float a     = (-B + sqrt(B*B - 4.0*A*C)) / (2.0*A);
    vec2 uv     = (point - a*sinangle) / cosangle;
    float r     = FIX(registers.R*acos(a));
    
    return uv*r/sin(r/registers.R);
}

vec2 fwtrans(vec2 uv)
{
    float r = FIX(sqrt(dot(uv, uv)));
    uv *= sin(r/registers.R)/r;
    float x = 1.0 - cos(r/registers.R);
    float D;
    
    if(registers.vertical_scanlines < 0.5)
      D = registers.d/registers.R + x*cosangle.x*cosangle.y + dot(uv,sinangle);
    else
      D = registers.d/registers.R + x*cosangle.y*cosangle.x + dot(uv,sinangle);

    return registers.d*(uv*cosangle - x*sinangle)/D;
}

vec3 maxscale()
{
   if(registers.vertical_scanlines < 0.5)
   {
       vec2 c = bkwtrans(-registers.R * sinangle / (1.0 + registers.R/registers.d*cosangle.x*cosangle.y));
       vec2 a = vec2(0.5, 0.5)*aspect;

       vec2 lo = vec2(fwtrans(vec2(-a.x,  c.y)).x,
                      fwtrans(vec2( c.x, -a.y)).y)/aspect;
       vec2 hi = vec2(fwtrans(vec2(+a.x,  c.y)).x,
                      fwtrans(vec2( c.x, +a.y)).y)/aspect;

       return vec3((hi+lo)*aspect*0.5,max(hi.x-lo.x, hi.y-lo.y));
   }else{
       vec2 c = bkwtrans(-registers.R * sinangle / (1.0 + registers.R/registers.d*cosangle.y*cosangle.x));
       vec2 a = vec2(0.5, 0.5)*aspect;

       vec2 lo = vec2(fwtrans(vec2(-a.y,  c.x)).y,
                      fwtrans(vec2( c.y, -a.x)).x)/aspect;
       vec2 hi = vec2(fwtrans(vec2(+a.y,  c.x)).y,
                      fwtrans(vec2( c.y, +a.x)).x)/aspect;

       return vec3((hi+lo)*aspect*0.5,max(hi.y-lo.y, hi.x-lo.x));
   }
}

// Calculate the influence of a scanline on the current pixel.
//
// 'distance' is the distance in texture coordinates from the current
// pixel to the scanline in question.
// 'color' is the colour of the scanline at the horizontal location of
// the current pixel.
vec4 scanlineWeights(float distance, vec4 color)
{
    // "wid" controls the width of the scanline beam, for each RGB
    // channel The "weights" lines basically specify the formula
    // that gives you the profile of the beam, i.e. the intensity as
    // a function of distance from the vertical center of the
    // scanline. In this case, it is gaussian if width=2, and
    // becomes nongaussian for larger widths. Ideally this should
    // be normalized so that the integral across the beam is
    // independent of its width. That is, for a narrower beam
    // "weights" should have a higher peak at the center of the
    // scanline than for a wider beam.
    #ifdef USEGAUSSIAN
        vec4 wid = 0.3 + 0.1 * pow(color, vec4(3.0));
        vec4 weights = vec4(distance / wid);
        
        return (registers.lum + 0.4) * exp(-weights * weights) / wid;
    #else
        vec4 wid = 2.0 + 2.0 * pow(color, vec4(4.0));
        vec4 weights = vec4(distance / registers.scanline_weight);
        
        return (registers.lum + 1.4) * exp(-pow(weights * inversesqrt(0.5 * wid), wid)) / (0.6 + 0.2 * wid);
    #endif
}

vec2 transform(vec2 coord)
{
    coord = (coord - vec2(0.5, 0.5))*aspect*stretch.z + stretch.xy;
    
    return (bkwtrans(coord) /
        vec2(registers.overscan_x / 100.0, registers.overscan_y / 100.0)/aspect + vec2(0.5, 0.5));
}

float corner(vec2 coord)
{
    coord = (coord - vec2(0.5)) * vec2(registers.overscan_x / 100.0, registers.overscan_y / 100.0) + vec2(0.5, 0.5);
    coord = min(coord, vec2(1.0) - coord) * aspect;
    vec2 cdist = vec2(registers.cornersize);
    coord = (cdist - min(coord, cdist));
    float dist = sqrt(dot(coord, coord));
    
    if(registers.vertical_scanlines < 0.5)
      return clamp((cdist.x - dist)*registers.cornersmooth, 0.0, 1.0);
    else
      return clamp((cdist.y - dist)*registers.cornersmooth, 0.0, 1.0);
}

// Masks adapted from cgwg's crt-geom-deluxe LUTs.
vec3 mask_weights_alpha(vec2 coord, float mask_intensity, int phosphor_layout, out float alpha){
   vec3 weights = vec3(1.,1.,1.);
   float on = 1.;
   float off = 1.-mask_intensity;
   vec3 red     = vec3(on,  off, off);// 1
   vec3 green   = vec3(off, on,  off);// 1
   vec3 blue    = vec3(off, off, on );// 1
   vec3 magenta = vec3(on,  off, on );// 2
   vec3 yellow  = vec3(on,  on,  off);// 2
   vec3 cyan    = vec3(off, on,  on );// 2
   vec3 black   = vec3(off, off, off);// 0
   vec3 white   = vec3(on,  on,  on );// 3
   int w, z = 0;
   alpha = 1.;
   
   // This pattern is used by a few layouts, so we'll define it here
   vec3 aperture_weights = mix(magenta, green, floor(mod(coord.x, 2.0)));
   
   if(phosphor_layout == 0) return weights;

   else if(phosphor_layout == 1){
      // classic aperture for RGB panels; good for 1080p, too small for 4K+
      // aka aperture_1_2_bgr
      weights.rgb  = aperture_weights;
	  alpha = 3./6.;
      return weights;
   }

   else if(phosphor_layout == 2){
      // 2x2 shadow mask for RGB panels; good for 1080p, too small for 4K+
      // aka delta_1_2x1_bgr
      vec3 inverse_aperture = mix(green, magenta, floor(mod(coord.x, 2.0)));
      weights               = mix(aperture_weights, inverse_aperture, floor(mod(coord.y, 2.0)));
      alpha = 6./12.;
      return weights;
   }

   else if(phosphor_layout == 3){
      // slot mask for RGB panels; looks okay at 1080p, looks better at 4K
      vec3 slotmask[3][4] = {
         {magenta, green, black,   black},
         {magenta, green, magenta, green},
         {black,   black, magenta, green}
      };
      
      // find the vertical index
      w = int(floor(mod(coord.y, 3.0)));

      // find the horizontal index
      z = int(floor(mod(coord.x, 4.0)));

      // use the indexes to find which color to apply to the current pixel
      weights = slotmask[w][z];
      alpha = 12./36.;
      return weights;
   }

   else if(phosphor_layout == 4){
      // classic aperture for RBG panels; good for 1080p, too small for 4K+
      weights  = mix(yellow, blue, floor(mod(coord.x, 2.0)));
      alpha = 3./6.;
      return weights;
   }

   else if(phosphor_layout == 5){
      // 2x2 shadow mask for RBG panels; good for 1080p, too small for 4K+
      vec3 inverse_aperture = mix(blue, yellow, floor(mod(coord.x, 2.0)));
      weights               = mix(mix(yellow, blue, floor(mod(coord.x, 2.0))), inverse_aperture, floor(mod(coord.y, 2.0)));
      alpha = 6./12.;
      return weights;
   }
   
   else if(phosphor_layout == 6){
      // aperture_1_4_rgb; good for simulating lower 
      vec3 ap4[4] = vec3[](red, green, blue, black);
      
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = ap4[z];
      alpha = 3./12.;
      return weights;
   }
   
   else if(phosphor_layout == 7){
      // aperture_2_5_bgr
      vec3 ap3[5] = vec3[](red, magenta, blue, green, green);
      
      z = int(floor(mod(coord.x, 5.0)));
      
      weights = ap3[z];
      alpha = 6./15.;
      return weights;
   }
   
   else if(phosphor_layout == 8){
      // aperture_3_6_rgb
      
      vec3 big_ap[7] = vec3[](red, red, yellow, green, cyan, blue, blue);
      
      w = int(floor(mod(coord.x, 7.)));
      
      weights = big_ap[w];
      alpha = 8./18.;
      return weights;
   }
   
   else if(phosphor_layout == 9){
      // reduced TVL aperture for RGB panels
      // aperture_2_4_rgb
      
      vec3 big_ap_rgb[4] = vec3[](red, yellow, cyan, blue);
      
      w = int(floor(mod(coord.x, 4.)));
      
      weights = big_ap_rgb[w];
      alpha = 6./12.;
      return weights;
   }
   
   else if(phosphor_layout == 10){
      // reduced TVL aperture for RBG panels
      
      vec3 big_ap_rbg[4] = vec3[](red, magenta, cyan, green);
      
      w = int(floor(mod(coord.x, 4.)));
      
      weights = big_ap_rbg[w];
      alpha = 6./12.;
      return weights;
   }
   
   else if(phosphor_layout == 11){
      // delta_1_4x1_rgb; dunno why this is called 4x1 when it's obviously 4x2 /shrug
      vec3 delta1[2][4] = {
         {red,  green, blue, black},
         {blue, black, red,  green}
      };
      
      w = int(floor(mod(coord.y, 2.0)));
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = delta1[w][z];
      alpha = 6./24.;
      return weights;
   }
   
   else if(phosphor_layout == 12){
      // delta_2_4x1_rgb
      vec3 delta[2][4] = {
         {red, yellow, cyan, blue},
         {cyan, blue, red, yellow}
      };
      
      w = int(floor(mod(coord.y, 2.0)));
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = delta[w][z];
      alpha = 12./24.;
      return weights;
   }
   
   else if(phosphor_layout == 13){
      // delta_2_4x2_rgb
      vec3 delta[4][4] = {
         {red,  yellow, cyan, blue},
         {red,  yellow, cyan, blue},
         {cyan, blue,   red,  yellow},
         {cyan, blue,   red,  yellow}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = delta[w][z];
      alpha = 24./48.;
      return weights;
   }

   else if(phosphor_layout == 14){
      // slot mask for RGB panels; too low-pitch for 1080p, looks okay at 4K, but wants 8K+
      vec3 slotmask[3][6] = {
         {magenta, green, black, black,   black, black},
         {magenta, green, black, magenta, green, black},
         {black,   black, black, magenta, green, black}
      };
      
      w = int(floor(mod(coord.y, 3.0)));

      z = int(floor(mod(coord.x, 6.0)));

      weights = slotmask[w][z];
      alpha = 12./54.;
      return weights;
   }
   
   else if(phosphor_layout == 15){
      // slot_2_4x4_rgb
      vec3 slot2[4][8] = {
         {red,   yellow, cyan,  blue,  red,   yellow, cyan,  blue },
         {red,   yellow, cyan,  blue,  black, black,  black, black},
         {red,   yellow, cyan,  blue,  red,   yellow, cyan,  blue },
         {black, black,  black, black, red,   yellow, cyan,  blue }
      };
   
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 8.0)));
      
      weights = slot2[w][z];
      alpha = 36./96.;
      return weights;
   }

   else if(phosphor_layout == 16){
      // slot mask for RBG panels; too low-pitch for 1080p, looks okay at 4K, but wants 8K+
      vec3 slotmask[3][4] = {
         {yellow, blue,  black,  black},
         {yellow, blue,  yellow, blue},
         {black,  black, yellow, blue}
      };
      
      w = int(floor(mod(coord.y, 3.0)));

      z = int(floor(mod(coord.x, 4.0)));

      weights = slotmask[w][z];
      alpha = 14./36.;
      return weights;
   }
   
   else if(phosphor_layout == 17){
      // slot_2_5x4_bgr
      vec3 slot2[4][10] = {
         {red,   magenta, blue,  green, green, red,   magenta, blue,  green, green},
         {black, blue,    blue,  green, green, red,   red,     black, black, black},
         {red,   magenta, blue,  green, green, red,   magenta, blue,  green, green},
         {red,   red,     black, black, black, black, blue,    blue,  green, green}
      };
   
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 10.0)));
      
      weights = slot2[w][z];
      alpha = 36./120.;
      return weights;
   }
   
   else if(phosphor_layout == 18){
      // same as above but for RBG panels
      vec3 slot2[4][10] = {
         {red,   yellow, green, blue,  blue,  red,   yellow, green, blue,  blue },
         {black, green,  green, blue,  blue,  red,   red,    black, black, black},
         {red,   yellow, green, blue,  blue,  red,   yellow, green, blue,  blue },
         {red,   red,    black, black, black, black, green,  green, blue,  blue }
      };
   
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 10.0)));
      
      weights = slot2[w][z];
      alpha = 36./120.;
      return weights;
   }
   
   else if(phosphor_layout == 19){
      // slot_3_7x6_rgb
      vec3 slot[6][14] = {
         {red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue},
         {red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue},
         {red,   red,   yellow, green, cyan,  blue,  blue,  black, black, black,  black,  black, black, black},
         {red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue},
         {red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue},
         {black, black, black,  black, black, black, black, black, red,   red,    yellow, green, cyan,  blue}
      };
      
      w = int(floor(mod(coord.y, 6.0)));
      z = int(floor(mod(coord.x, 14.0)));
      
      weights = slot[w][z];
      alpha = 89./252.; // 49+(2*20)
      return weights;
   }

   else if(phosphor_layout == 20){
      // TATE slot mask for RGB layouts; this is not realistic obviously, but it looks nice and avoids chromatic aberration
      vec3 tatemask[4][4] = {
         {green, magenta, green, magenta},
         {black, blue,    green, red},
         {green, magenta, green, magenta},
         {green, red,     black, blue}
      };
      
      w = int(floor(mod(coord.y, 4.0)));

      z = int(floor(mod(coord.x, 4.0)));

      weights = tatemask[w][z];
      alpha = 18./48.;
      return weights;
   }
   
   else if(phosphor_layout == 21){
      // based on MajorPainInTheCactus' HDR slot mask
      vec3 slot[4][8] = {
         {red,   green, blue,  black, red,   green, blue,  black},
         {red,   green, blue,  black, black, black, black, black},
         {red,   green, blue,  black, red,   green, blue,  black},
         {black, black, black, black, red,   green, blue,  black}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 8.0)));
      
      weights = slot[w][z];
      alpha = 21./96.;
      return weights;
   }
	
   else if(phosphor_layout == 22){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 1080p and lower
      vec3 bw3[3] = vec3[](black, white, white);
      
      z = int(floor(mod(coord.x, 3.0)));
      
      weights = bw3[z];
      alpha = 2./3.;
      return weights;
   }

   else if(phosphor_layout == 23){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 4k 
      vec3 bw4[4] = vec3[](black, black, white, white);
      
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = bw4[z];
      alpha = 0.5;
      return weights;
   }
   
   else if(phosphor_layout == 24){
      // shadowmask courtesy of Louis. Suitable for lower TVL on high-res 4K+ screens
      vec3 shadow[6][10] = {
         {green, cyan, blue, blue, blue, red, red, red, yellow, green},
         {green, cyan, blue, blue, blue, red, red, red, yellow, green},
         {green, cyan, blue, blue, blue, red, red, red, yellow, green},
         {red, red, red, yellow, green, green, cyan, blue, blue, blue},
         {red, red, red, yellow, green, green, cyan, blue, blue, blue},
         {red, red, red, yellow, green, green, cyan, blue, blue, blue},
      };
      
      w = int(floor(mod(coord.y, 6.0)));
      z = int(floor(mod(coord.x, 10.0)));
      
      weights = shadow[w][z];
      alpha = 72./180.;
      return weights;
   }

   else return weights;
}

void main()
{
    // Here's a helpful diagram to keep in mind while trying to
    // understand the code:
    //
    //  |      |      |      |      |
    // -------------------------------
    //  |      |      |      |      |
    //  |  01  |  11  |  21  |  31  | <-- current scanline
    //  |      | @    |      |      |
    // -------------------------------
    //  |      |      |      |      |
    //  |  02  |  12  |  22  |  32  | <-- next scanline
    //  |      |      |      |      |
    // -------------------------------
    //  |      |      |      |      |
    //
    // Each character-cell represents a pixel on the output
    // surface, "@" represents the current pixel (always somewhere
    // in the bottom half of the current scan-line, or the top-half
    // of the next scanline). The grid of lines represents the
    // edges of the texels of the underlying texture.

    // Texture coordinates of the texel containing the active pixel.
	vec2 xy;
	if (registers.CURVATURE > 0.5)
      xy = transform(vTexCoord);
	else
      xy = vTexCoord;

    float cval = corner(xy);

    // Of all the pixels that are mapped onto the texel we are
    // currently rendering, which pixel are we currently rendering?
   vec2 ilvec;
   if(registers.vertical_scanlines < 0.5)
      ilvec = vec2(0.0, ilfac.y * registers.interlace_detect > 1.5 ? mod(float(registers.FrameCount), 2.0) : 0.0);
   else
      ilvec = vec2(ilfac.x * registers.interlace_detect > 1.5 ? mod(float(registers.FrameCount), 2.0) : 0.0, 0.0);

    vec2 ratio_scale = (xy * TextureSize - vec2(0.5, 0.5) + ilvec) / ilfac;
    vec2 uv_ratio = fract(ratio_scale);

    // Snap to the center of the underlying texel.
    xy = (floor(ratio_scale)*ilfac + vec2(0.5, 0.5) - ilvec) / TextureSize;

    // Calculate Lanczos scaling coefficients describing the effect
    // of various neighbour texels in a scanline on the current
    // pixel.
    vec4 coeffs;
    if(registers.vertical_scanlines < 0.5)
      coeffs = PI * vec4(1.0 + uv_ratio.x, uv_ratio.x, 1.0 - uv_ratio.x, 2.0 - uv_ratio.x);
    else
      coeffs = PI * vec4(1.0 + uv_ratio.y, uv_ratio.y, 1.0 - uv_ratio.y, 2.0 - uv_ratio.y);

    // Prevent division by zero.
    coeffs = FIX(coeffs);

    // Lanczos2 kernel.
    coeffs = 2.0 * sin(coeffs) * sin(coeffs / 2.0) / (coeffs * coeffs);

    // Normalize.
    coeffs /= dot(coeffs, vec4(1.0));

    // Calculate the effective colour of the current and next
    // scanlines at the horizontal location of the current pixel,
    // using the Lanczos coefficients above.
    vec4 col, col2;
    if(registers.vertical_scanlines < 0.5)
    {
       col = clamp(
           mat4(
               TEX2D(xy + vec2(-one.x, 0.0)),
               TEX2D(xy),
               TEX2D(xy + vec2(one.x, 0.0)),
               TEX2D(xy + vec2(2.0 * one.x, 0.0))
           ) * coeffs,
           0.0, 1.0
       );
       col2 = clamp(
           mat4(
               TEX2D(xy + vec2(-one.x, one.y)),
               TEX2D(xy + vec2(0.0, one.y)),
               TEX2D(xy + one),
               TEX2D(xy + vec2(2.0 * one.x, one.y))
           ) * coeffs,
           0.0, 1.0
       );
    }else{
       col = clamp(
           mat4(
               TEX2D(xy + vec2(0.0, -one.y)),
               TEX2D(xy),
               TEX2D(xy + vec2(0.0, one.y)),
               TEX2D(xy + vec2(0.0, 2.0 * one.y))
           ) * coeffs,
           0.0, 1.0
       );
       col2 = clamp(
           mat4(
               TEX2D(xy + vec2(one.x, -one.y)),
               TEX2D(xy + vec2(one.x, 0.0)),
               TEX2D(xy + one),
               TEX2D(xy + vec2(one.x, 2.0 * one.y))
           ) * coeffs,
           0.0, 1.0
       );
    }

#ifndef LINEAR_PROCESSING
    col  = pow(col , vec4(registers.CRTgamma));
    col2 = pow(col2, vec4(registers.CRTgamma));
#endif

    // Calculate the influence of the current and next scanlines on
    // the current pixel.
    vec4 weights, weights2;
    if(registers.vertical_scanlines < 0.5)
    {
       weights  = scanlineWeights(uv_ratio.y, col);
       weights2 = scanlineWeights(1.0 - uv_ratio.y, col2);

   #ifdef OVERSAMPLE
       float filter_ = fwidth(ratio_scale.y);
       uv_ratio.y    = uv_ratio.y + 1.0/3.0*filter_;
       weights       = (weights  + scanlineWeights(uv_ratio.y, col))/3.0;
       weights2      = (weights2 + scanlineWeights(abs(1.0 - uv_ratio.y), col2))/3.0;
       uv_ratio.y    = uv_ratio.y - 2.0/3.0*filter_;
       weights       = weights  + scanlineWeights(abs(uv_ratio.y), col)/3.0;
       weights2      = weights2 + scanlineWeights(abs(1.0 - uv_ratio.y), col2)/3.0;
   #endif
    }else{
       weights  = scanlineWeights(uv_ratio.x, col);
       weights2 = scanlineWeights(1.0 - uv_ratio.x, col2);

   #ifdef OVERSAMPLE
       float filter_ = fwidth(ratio_scale.x);
       uv_ratio.x    = uv_ratio.x + 1.0/3.0*filter_;
       weights       = (weights  + scanlineWeights(uv_ratio.x, col))/3.0;
       weights2      = (weights2 + scanlineWeights(abs(1.0 - uv_ratio.x), col2))/3.0;
       uv_ratio.x    = uv_ratio.x - 2.0/3.0*filter_;
       weights       = weights  + scanlineWeights(abs(uv_ratio.x), col)/3.0;
       weights2      = weights2 + scanlineWeights(abs(1.0 - uv_ratio.x), col2)/3.0;
   #endif
    }

    vec3 mul_res  = (col * weights + col2 * weights2).rgb * vec3(cval);
    
    // Shadow mask
    // original code; just makes a giant phosphor here
    xy = fract(vTexCoord.xy * u_quad_dims.xy / u_tex_size1.xy);

    // use subpixel mask code instead of LUTs
    vec4 mask = vec4(1.0);
    float alpha;
    if(registers.vertical_scanlines < 0.5)
      mask = vec4(mask_weights_alpha(vTexCoord.xy * global.OutputSize.xy, 1., int(registers.mask_type), alpha), 1.0);
    else
      mask = vec4(mask_weights_alpha(vTexCoord.yx * global.OutputSize.yx, 1., int(registers.mask_type), alpha), 1.0);
    mask.a = alpha;

    // count of total bright pixels is encoded in the mask's alpha channel
    float nbright = 255.0 - 255.0*mask.a;
    // fraction of bright pixels in the mask
    float fbright = nbright / ( u_tex_size1.x * u_tex_size1.y );
    // average darkening factor of the mask
    float aperture_average = mix(1.0-DOTMASK.x*(1.0-DOTMASK_brightboost.x), 1.0, fbright);
    // colour of dark mask pixels
    vec3 clow = vec3(1.0-DOTMASK.x) * mul_res + vec3(DOTMASK.x*(DOTMASK_brightboost.x)) * mul_res * mul_res;
    float ifbright = 1.0 / fbright;
    // colour of bright mask pixels
    vec3 chi = vec3(ifbright*aperture_average) * mul_res - vec3(ifbright - 1.0) * clow;
    vec3 cout = mix(clow,chi,mask.rgb); // mask texture selects dark vs bright

    // Convert the image gamma for display on our output device.
    mul_res = pow(cout, vec3(1.0 / registers.monitorgamma));

    FragColor = vec4(mul_res, 1.0);
}
