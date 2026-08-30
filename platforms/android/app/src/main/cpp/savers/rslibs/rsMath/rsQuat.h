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

#ifndef RSQUAT_H
#define RSQUAT_H

class rsVec;

class rsQuat
{
public:
	float q[4];

	rsQuat();
	rsQuat(float x, float y, float z, float w);
	~rsQuat();

	void set(float x, float y, float z, float w);	// x, y, z, w
	void copy(rsQuat);						// Copy another quaternion
	void make(float a, float x, float y, float z);	// angle, normalized axis
	void make(float a, const rsVec &v);  	// angle, normalized axis
	void normalize();
	void preMult(rsQuat &postQuat);			// Multiply this quaternion by
											// the passed quaternion
											// (this * passed)
	void postMult(rsQuat &preQuat);			// Multiply the passed quaternion
											// by this quaternion
											// (passed * this)
	void toMat(float *);					// Convert quaternion to array of 16 floats
	void fromMat(float *);					// Convert array of 16 floats to quaternion
	void fromEuler(float, float, float);	// Convert from hpr angles
	void slerp(rsQuat, rsQuat, float);		// Interpolate first quaternion
											// to second using float from 0.0
											// to 1.0

	float & operator [] (int i)
	{
		return q[i];
	}

	const float & operator [] (int i) const
	{
		return q[i];
	}
};

#endif
