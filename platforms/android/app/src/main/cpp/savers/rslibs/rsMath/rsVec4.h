/*
 * Copyright (C) 1999-2010  Terence M. Welsh
 *
 * This file is part of rsMath.
 *
 * rsMath is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * rsMath is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef RSVEC4_H
#define RSVEC4_H

class rsMatrix;

class rsVec4
{
public:
	float v[4];

	rsVec4();
	rsVec4(float x, float y, float z, float w);
	virtual ~rsVec4();

	void set(float x, float y, float z, float w);
	float length();
	float normalize();
	float dot(rsVec4);
	void cross(rsVec4, rsVec4);
	void scale(float);
	void transPoint(const rsMatrix &m);
	void transVec(const rsMatrix &m);
	int almostEqual(rsVec4 vec, float tolerance);

	float & operator [] (int i)
	{
		return v[i];
	}

	const float & operator [] (int i) const
	{
		return v[i];
	}

	rsVec4 & operator = (const rsVec4 &vec)
	{
		v[0] = vec[0]; v[1] = vec[1]; v[2] = vec[2]; v[3] = vec[3]; return *this;
	}

	rsVec4 operator + (const rsVec4 &vec)
	{
		return(rsVec4(v[0] + vec[0], v[1] + vec[1], v[2] + vec[2], v[3] + vec[3]));
	}

	rsVec4 operator - (const rsVec4 &vec)
	{
		return(rsVec4(v[0] - vec[0], v[1] - vec[1], v[2] - vec[2], v[3] - vec[3]));
	}

	rsVec4 operator * (const float &mul)
	{
		return(rsVec4(v[0] * mul, v[1] * mul, v[2] * mul, v[3] * mul));
	}

	rsVec4 operator / (const float &div)
	{
		float rec = 1.0f / div; return(rsVec4(v[0] * rec, v[1] * rec, v[2] * rec, v[3] * rec));
	}

	rsVec4 & operator += (const rsVec4 &vec)
	{
		v[0] += vec[0]; v[1] += vec[1]; v[2] += vec[2]; v[3] += vec[3]; return *this;
	}

	rsVec4 & operator -= (const rsVec4 &vec)
	{
		v[0] -= vec[0]; v[1] -= vec[1]; v[2] -= vec[2]; v[3] -= vec[3]; return *this;
	}

	rsVec4 & operator *= (const rsVec4 &vec)
	{
		v[0] *= vec[0]; v[1] *= vec[1]; v[2] *= vec[2]; v[3] *= vec[3]; return *this;
	}

	rsVec4 & operator *= (const float &mul)
	{
		v[0] *= mul; v[1] *= mul; v[2] *= mul; v[3] *= mul; return *this;
	}
};

#endif
