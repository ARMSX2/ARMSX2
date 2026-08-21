/*
 * Copyright (C) 1999-2010  Terence M. Welsh
 *
 * This file is part of Rgbhsl.
 *
 * Rgbhsl is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Rgbhsl is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

 // This library converts between colors defined with RGB values and HSL
 // values.  It also finds in-between colors by moving linearly through
 // HSL space.
 // All functions take values for r, g, b, h, s, and l between 0.0 and 1.0
 // (RGB = red, green, blue;  HSL = hue, saturation, luminosity)

#ifndef RGBHSL_H
#define RGBHSL_H

void rgb2hsl(float r, float g, float b, float &h, float &s, float &l);

void hsl2rgb(float h, float s, float l, float &r, float &g, float &b);

// For these 'tween functions, a tween value of 0.0 will output the first
// color while a tween value of 1.0 will output the second color.
// A value of 0 for direction indicates a positive progression around
// the color wheel (i.e. red -> yellow -> green -> cyan...).  A value of
// 1 does the opposite.
void hslTween(float h1, float s1, float l1,
	float h2, float s2, float l2, float tween, int direction,
	float &outh, float &outs, float &outl);

void rgbTween(float r1, float g1, float b1,
	float r2, float g2, float b2, float tween, int direction,
	float &outr, float &outg, float &outb);

#endif // RGBHSL_H
