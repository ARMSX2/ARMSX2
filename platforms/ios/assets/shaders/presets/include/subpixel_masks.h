/*
A collection of CRT mask effects that work with LCD subpixel structures for
small details

author: hunterk
license: public domain

How to use it:

Multiply your image by the vec3 output:
FragColor.rgb *= mask_weights(gl_FragCoord.xy, 1.0, 1);

In the "alpha" version, the alpha channel stores the number of lit subpixels per pixel for use in brightness-loss compensation efforts.

The function needs to be tiled across the screen using the physical pixels, e.g.
gl_FragCoord (the "vec2 coord" input). In the case of slang shaders, we use
(vTexCoord.st * OutputSize.xy).

The "mask_intensity" (float value between 0.0 and 1.0) is how strong the mask
effect should be. Full-strength red, green and blue subpixels on a white pixel
are the ideal, and are achieved with an intensity of 1.0, though this darkens
the image significantly and may not always be desirable.

The "phosphor_layout" (int value between 0 and 24) determines which phophor
layout to apply. 0 is no mask/passthru.

Many of these mask arrays are adapted from cgwg's crt-geom-deluxe LUTs, and
those have their filenames included for easy identification
*/

vec3 mask_weights(vec2 coord, float mask_intensity, int phosphor_layout){
   vec3 weights = vec3(1.,1.,1.);
   float on = 1.;
   float off = 1.-mask_intensity;
   vec3 red     = vec3(on,  off, off);
   vec3 green   = vec3(off, on,  off);
   vec3 blue    = vec3(off, off, on );
   vec3 magenta = vec3(on,  off, on );
   vec3 yellow  = vec3(on,  on,  off);
   vec3 cyan    = vec3(off, on,  on );
   vec3 black   = vec3(off, off, off);
   vec3 white   = vec3(on,  on,  on );
   int w, z = 0;
   
   // This pattern is used by a few layouts, so we'll define it here
   vec3 aperture_weights = mix(magenta, green, floor(mod(coord.x, 2.0)));
   
   if(phosphor_layout == 0) return weights;

   else if(phosphor_layout == 1){
      // classic aperture for RGB panels; good for 1080p, too small for 4K+
      // aka aperture_1_2_bgr
      weights  = aperture_weights;
      return weights;
   }

   else if(phosphor_layout == 2){
      // 2x2 shadow mask for RGB panels; good for 1080p, too small for 4K+
      // aka delta_1_2x1_bgr
      vec3 inverse_aperture = mix(green, magenta, floor(mod(coord.x, 2.0)));
      weights               = mix(aperture_weights, inverse_aperture, floor(mod(coord.y, 2.0)));
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
      return weights;
   }

   else if(phosphor_layout == 4){
      // classic aperture for RBG panels; good for 1080p, too small for 4K+
      weights  = mix(yellow, blue, floor(mod(coord.x, 2.0)));
      return weights;
   }

   else if(phosphor_layout == 5){
      // 2x2 shadow mask for RBG panels; good for 1080p, too small for 4K+
      vec3 inverse_aperture = mix(blue, yellow, floor(mod(coord.x, 2.0)));
      weights               = mix(mix(yellow, blue, floor(mod(coord.x, 2.0))), inverse_aperture, floor(mod(coord.y, 2.0)));
      return weights;
   }
   
   else if(phosphor_layout == 6){
      // aperture_1_4_rgb; good for simulating lower 
      vec3 ap4[4] = vec3[](red, green, blue, black);
      
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = ap4[z];
      return weights;
   }
   
   else if(phosphor_layout == 7){
      // aperture_2_5_bgr
      vec3 ap3[5] = vec3[](red, magenta, blue, green, green);
      
      z = int(floor(mod(coord.x, 5.0)));
      
      weights = ap3[z];
      return weights;
   }
   
   else if(phosphor_layout == 8){
      // aperture_3_6_rgb
      
      vec3 big_ap[7] = vec3[](red, red, yellow, green, cyan, blue, blue);
      
      w = int(floor(mod(coord.x, 7.)));
      
      weights = big_ap[w];
      return weights;
   }
   
   else if(phosphor_layout == 9){
      // reduced TVL aperture for RGB panels
      // aperture_2_4_rgb
      
      vec3 big_ap_rgb[4] = vec3[](red, yellow, cyan, blue);
      
      w = int(floor(mod(coord.x, 4.)));
      
      weights = big_ap_rgb[w];
      return weights;
   }
   
   else if(phosphor_layout == 10){
      // reduced TVL aperture for RBG panels
      
      vec3 big_ap_rbg[4] = vec3[](red, magenta, cyan, green);
      
      w = int(floor(mod(coord.x, 4.)));
      
      weights = big_ap_rbg[w];
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
      return weights;
   }
	
   else if(phosphor_layout == 22){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 1080p and lower
      vec3 bw3[3] = vec3[](black, white, white);
      
      z = int(floor(mod(coord.x, 3.0)));
      
      weights = bw3[z];
      return weights;
   }

   else if(phosphor_layout == 23){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 4k 
      vec3 bw4[4] = vec3[](black, black, white, white);
      
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = bw4[z];
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
      return weights;
   }

   else return weights;
}

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
