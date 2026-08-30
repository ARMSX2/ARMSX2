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

#ifndef RSVEC_H
#define RSVEC_H

class rsMatrix;

class rsVec
{
public:
	float v[3];

	rsVec();
	rsVec(float xx, float yy, float zz);
	virtual ~rsVec();

	void set(float xx, float yy, float zz);
	float length();
	float normalize();
	float dot(rsVec);
	void cross(rsVec, rsVec);
	void scale(float);
	void transPoint(const rsMatrix &m);
	void transVec(const rsMatrix &m);
	int almostEqual(rsVec vec, float tolerance);

	float & operator [] (int i)
	{
		return v[i];
	}

	const float & operator [] (int i) const
	{
		return v[i];
	}

	rsVec & operator = (const rsVec &vec)
	{
		v[0] = vec[0]; v[1] = vec[1]; v[2] = vec[2]; return *this;
	}

	rsVec operator + (const rsVec &vec)
	{
		return(rsVec(v[0] + vec[0], v[1] + vec[1], v[2] + vec[2]));
	}

	rsVec operator - (const rsVec &vec)
	{
		return(rsVec(v[0] - vec[0], v[1] - vec[1], v[2] - vec[2]));
	}

	rsVec operator * (const float &mul)
	{
		return(rsVec(v[0] * mul, v[1] * mul, v[2] * mul));
	}

	rsVec operator / (const float &div)
	{
		float rec = 1.0f / div; return(rsVec(v[0] * rec, v[1] * rec, v[2] * rec));
	}

	rsVec & operator += (const rsVec &vec)
	{
		v[0] += vec[0]; v[1] += vec[1]; v[2] += vec[2]; return *this;
	}

	rsVec & operator -= (const rsVec &vec)
	{
		v[0] -= vec[0]; v[1] -= vec[1]; v[2] -= vec[2]; return *this;
	}

	rsVec & operator *= (const rsVec &vec)
	{
		v[0] *= vec[0]; v[1] *= vec[1]; v[2] *= vec[2]; return *this;
	}

	rsVec & operator *= (const float &mul)
	{
		v[0] *= mul; v[1] *= mul; v[2] *= mul; return *this;
	}
};

// Generic C-style vector math functions
float rsLength(const float *xyz);
float rsNormalize(float *xyz);
float rsDot(const float *xyz1, const float *xyz2);
void  rsCross(const float *xyz1, const float *xyz2, float *xyzOut);
void  rsScaleVec(float *xyz, float scale);

// Backward-compatible overloads with non-const input pointers
inline float rsLength(float *xyz)
{
	return rsLength(static_cast<const float*>(xyz));
}

inline float rsDot(float *xyz1, float *xyz2)
{
	return rsDot(static_cast<const float*>(xyz1), static_cast<const float*>(xyz2));
}

inline void rsCross(float *xyz1, float *xyz2, float *xyzOut)
{
	rsCross(static_cast<const float*>(xyz1), static_cast<const float*>(xyz2), xyzOut);
}

#endif
