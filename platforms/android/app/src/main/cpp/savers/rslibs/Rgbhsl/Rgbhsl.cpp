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

#include "Rgbhsl.h"

#include <cmath>

static constexpr float kEpsilon    = 1e-7f;
static constexpr float kOneSixth   = 1.0f / 6.0f;
static constexpr float kOneThird   = 1.0f / 3.0f;
static constexpr float kOneHalf    = 0.5f;
static constexpr float kTwoThirds  = 2.0f / 3.0f;
static constexpr float kFiveSixths = 5.0f / 6.0f;

void
rgb2hsl(float r, float g, float b, float &h, float &s, float &l)
{
	int huezone = 0;
	float rr, gg, bb;

	// find huezone
	if (r >= g)
	{
		huezone = 0;
		if (b > r)
			huezone = 4;
		else
		{
			if (b > g)
				huezone = 5;
		}
	}
	else
	{
		huezone = 1;
		if (b > g)
			huezone = 2;
		else
		{
			if (b > r)
				huezone = 3;
		}
	}

	// luminosity
	switch (huezone)
	{
		case 0:
		case 5:
			l = r;
			break;
		case 1:
		case 3:
			l = g;
			break;
		default:
			l = b;
	}

	if (l < kEpsilon)
	{
		// Near-black: l is very small; set achromatic output so round-trips stay grayscale.
		h = 0.0f;
		s = 0.0f;
		return;
	}

	// Normalize channels by luminosity so hue/saturation do not depend on brightness.
	switch (huezone)
	{
		case 0:
		case 5:
			rr = 1.0f;
			gg = g / l;
			bb = b / l;
			break;
		case 1:
		case 3:
			gg = 1.0f;
			rr = r / l;
			bb = b / l;
			break;
		default:
			bb = 1.0f;
			rr = r / l;
			gg = g / l;
	}

	// saturation
	switch (huezone)
	{
		case 0:
		case 1:
			s = 1.0f - bb;
			if (s < kEpsilon)
			{
				s = 0.0f;
				h = 0.0f;
				return;
			}
			bb = 0.0f;
			rr = 1.0f - ((1.0f - rr) / s);
			gg = 1.0f - ((1.0f - gg) / s);
			break;
		case 2:
		case 3:
			s = 1.0f - rr;
			if (s < kEpsilon)
			{
				s = 0.0f;
				h = 0.0f;
				return;
			}
			rr = 0.0f;
			gg = 1.0f - ((1.0f - gg) / s);
			bb = 1.0f - ((1.0f - bb) / s);
			break;
		default:
			s = 1.0f - gg;
			if (s < kEpsilon)
			{
				s = 0.0f;
				h = 0.0f;
				return;
			}
			gg = 0.0f;
			rr = 1.0f - ((1.0f - rr) / s);
			bb = 1.0f - ((1.0f - bb) / s);
	}

	// hue
	switch (huezone)
	{
		case 0:
			h = gg * kOneSixth;
			break;
		case 1:
			h = (1.0f - rr) * kOneSixth + kOneSixth;
			break;
		case 2:
			h = (1.0f - gg) * kOneSixth + kOneHalf;
			break;
		case 3:
			h = bb * kOneSixth + kOneThird;
			break;
		case 4:
			h = rr * kOneSixth + kTwoThirds;
			break;
		default:
			h = (1.0f - bb) * kOneSixth + kFiveSixths;
	}
}

void
hsl2rgb(float h, float s, float l, float &r, float &g, float &b)
{
	// hue influence
	h = std::fmod(h, 1.0f);
	if (h < 0.0f) h += 1.0f;
	if (h < kOneSixth)
	{  // full red, some green
		r = 1.0f;
		g = h * 6.0f;
		b = 0.0f;
	}
	else
	{
		if (h < kOneHalf)
		{  // full green
			g = 1.0f;
			if (h < kOneThird)
			{  // some red
				r = 1.0f - ((h - kOneSixth) * 6.0f);
				b = 0.0f;
			}
			else
			{  // some blue
				b = (h - kOneThird) * 6.0f;
				r = 0.0f;
			}
		}
		else
		{
			if (h < kFiveSixths)
			{  // full blue
				b = 1.0f;
				if (h < kTwoThirds)
				{  // some green
					g = 1.0f - ((h - kOneHalf) * 6.0f);
					r = 0.0f;
				}
				else
				{  // some red
					r = (h - kTwoThirds) * 6.0f;
					g = 0.0f;
				}
			}
			else
			{  // full red, some blue
				r = 1.0f;
				b = 1.0f - ((h - kFiveSixths) * 6.0f);
				g = 0.0f;
			}
		}
	}

	// saturation influence
	r = 1.0f - (s * (1.0f - r));
	g = 1.0f - (s * (1.0f - g));
	b = 1.0f - (s * (1.0f - b));

	// luminosity influence
	r *= l;
	g *= l;
	b *= l;
}

void
hslTween(float h1, float s1, float l1,
	float h2, float s2, float l2, float tween, int direction,
	float &outh, float &outs, float &outl)
{
	// hue
	if (!direction)
	{  // forward around color wheel
		if (h2 >= h1)
			outh = h1 + (tween * (h2 - h1));
		else
		{
			outh = h1 + (tween * (1.0f - (h1 - h2)));
			if (outh > 1.0f)
				outh -= 1.0f;
		}
	}
	else
	{  // backward around color wheel
		if (h1 >= h2)
			outh = h1 - (tween * (h1 - h2));
		else
		{
			outh = h1 - (tween * (1.0f - (h2 - h1)));
			if (outh < 0.0f)
				outh += 1.0f;
		}
	}

	// saturation
	outs = s1 + (tween * (s2 - s1));

	// luminosity
	outl = l1 + (tween * (l2 - l1));
}

void
rgbTween(float r1, float g1, float b1,
	float r2, float g2, float b2, float tween, int direction,
	float &outr, float &outg, float &outb)
{
	float h1, s1, l1, h2, s2, l2, outh, outs, outl;

	rgb2hsl(r1, g1, b1, h1, s1, l1);
	rgb2hsl(r2, g2, b2, h2, s2, l2);
	hslTween(h1, s1, l1, h2, s2, l2, tween, direction, outh, outs, outl);
	hsl2rgb(outh, outs, outl, outr, outg, outb);
}
