#version 450

/*
   SameBoy LCD shader
   Author: LIJI32
   License: MIT

   Copyright (c) 2015-2016 Lior Halphon

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

layout(std140, set = 0, binding = 0) uniform UBO
{
    vec4 SourceSize;
    vec4 OriginalSize;
    vec4 OutputSize;
    mat4 MVP;
} global;

layout(push_constant) uniform Push
{
    float COLOR_LOW;
    float COLOR_HIGH;
    float SCANLINE_DEPTH;
} params;

#pragma parameter COLOR_LOW "Color Low" 0.8 0.0 1.5 0.05
#pragma parameter COLOR_HIGH "Color High" 1.0 0.0 1.5 0.05
#pragma parameter SCANLINE_DEPTH "Scanline Depth" 0.1 0.0 2.0 0.05

#define COLOR_LOW params.COLOR_LOW
#define COLOR_HIGH params.COLOR_HIGH
#define SCANLINE_DEPTH params.SCANLINE_DEPTH

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
    vec2 pos = fract(vTexCoord * global.SourceSize.xy);
    vec2 sub_pos = fract(vTexCoord * global.SourceSize.xy * 6);
    
    vec4 center = texture(Source, vTexCoord);
    vec4 left = texture(Source, vTexCoord - vec2(1.0 / global.SourceSize.x, 0));
    vec4 right = texture(Source, vTexCoord + vec2(1.0 / global.SourceSize.x, 0));
    
    if (pos.y < 1.0 / 6.0) {
        center = mix(center, texture(Source, vTexCoord + vec2(0, -1.0 / global.SourceSize.y)), 0.5 - sub_pos.y / 2.0);
        left =   mix(left,   texture(Source, vTexCoord + vec2(-1.0 / global.SourceSize.x, -1.0 / global.SourceSize.y)), 0.5 - sub_pos.y / 2.0);
        right =  mix(right,  texture(Source, vTexCoord + vec2( 1.0 / global.SourceSize.x, -1.0 / global.SourceSize.y)), 0.5 - sub_pos.y / 2.0);
        center *= sub_pos.y * SCANLINE_DEPTH + (1 - SCANLINE_DEPTH);
        left *= sub_pos.y * SCANLINE_DEPTH + (1 - SCANLINE_DEPTH);
        right *= sub_pos.y * SCANLINE_DEPTH + (1 - SCANLINE_DEPTH);
    }
    else if (pos.y > 5.0 / 6.0) {
        center = mix(center, texture(Source, vTexCoord + vec2(0, 1.0 / global.SourceSize.y)), sub_pos.y / 2.0);
        left =   mix(left,   texture(Source, vTexCoord + vec2(-1.0 / global.SourceSize.x, 1.0 / global.SourceSize.y)), sub_pos.y / 2.0);
        right =  mix(right,  texture(Source, vTexCoord + vec2( 1.0 / global.SourceSize.x, 1.0 / global.SourceSize.y)), sub_pos.y / 2.0);
        center *= (1.0 - sub_pos.y) * SCANLINE_DEPTH + (1 - SCANLINE_DEPTH);
        left *= (1.0 - sub_pos.y) * SCANLINE_DEPTH + (1 - SCANLINE_DEPTH);
        right *= (1.0 - sub_pos.y) * SCANLINE_DEPTH + (1 - SCANLINE_DEPTH);
    }
    
    
    vec4 midleft = mix(left, center, 0.5);
    vec4 midright = mix(right, center, 0.5);
    
    vec4 ret;
    if (pos.x < 1.0 / 6.0) {
        ret = mix(vec4(COLOR_HIGH * center.r, COLOR_LOW * center.g, COLOR_HIGH * left.b, 1),
                  vec4(COLOR_HIGH * center.r, COLOR_LOW * center.g, COLOR_LOW  * left.b, 1),
                  sub_pos.x);
    }
    else if (pos.x < 2.0 / 6.0) {
        ret = mix(vec4(COLOR_HIGH * center.r, COLOR_LOW  * center.g, COLOR_LOW * left.b, 1),
                  vec4(COLOR_HIGH * center.r, COLOR_HIGH * center.g, COLOR_LOW * midleft.b, 1),
                  sub_pos.x);
    }
    else if (pos.x < 3.0 / 6.0) {
        ret = mix(vec4(COLOR_HIGH * center.r  , COLOR_HIGH * center.g, COLOR_LOW * midleft.b, 1),
                  vec4(COLOR_LOW  * midright.r, COLOR_HIGH * center.g, COLOR_LOW * center.b, 1),
                  sub_pos.x);
    }
    else if (pos.x < 4.0 / 6.0) {
        ret = mix(vec4(COLOR_LOW * midright.r, COLOR_HIGH * center.g , COLOR_LOW  * center.b, 1),
                  vec4(COLOR_LOW * right.r   , COLOR_HIGH  * center.g, COLOR_HIGH * center.b, 1),
                  sub_pos.x);
    }
    else if (pos.x < 5.0 / 6.0) {
        ret = mix(vec4(COLOR_LOW * right.r, COLOR_HIGH * center.g  , COLOR_HIGH * center.b, 1),
                  vec4(COLOR_LOW * right.r, COLOR_LOW  * midright.g, COLOR_HIGH * center.b, 1),
                  sub_pos.x);
    }
    else {
        ret = mix(vec4(COLOR_LOW  * right.r, COLOR_LOW * midright.g, COLOR_HIGH * center.b, 1),
                  vec4(COLOR_HIGH * right.r, COLOR_LOW * right.g  ,  COLOR_HIGH * center.b, 1),
                  sub_pos.x);
    }
    
    FragColor = ret;
}
