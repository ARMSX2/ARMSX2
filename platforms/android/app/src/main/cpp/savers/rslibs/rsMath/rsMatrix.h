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

#ifndef RSMATRIX_H
#define RSMATRIX_H

#include <ostream>

class rsVec;
class rsQuat;

class rsMatrix
{
public:
	float m[16];
	// 1 0 0 x   0 4 8  12  <-- Column order matrix just like OpenGL
	// 0 1 0 y   1 5 9  13
	// 0 0 1 z   2 6 10 14
	// 0 0 0 1   3 7 11 15

	rsMatrix();
	~rsMatrix();

	void identity();
	void set(float* mat);
	void get(float* mat);
	float* get();
	void copy(const rsMatrix &mat);
	void preMult(const rsMatrix &postMat);
	void postMult(const rsMatrix &preMat);
	void makeTranslate(float x, float y, float z);
	void makeTranslate(float* p);
	void makeTranslate(const rsVec &vec);
	void makeScale(float s);
	void makeScale(float x, float y, float z);
	void makeScale(float* s);
	void makeScale(const rsVec &vec);
	void makeRotate(float a, float x, float y, float z);  // normalized angle, axis
	void makeRotate(float a, const rsVec &v);  // normalized angle, axis
	void makeRotate(rsQuat &q);
	void translate(float x, float y, float z);
	void translate(float* p);
	void translate(const rsVec &vec);
	void scale(float s);
	void scale(float x, float y, float z);
	void scale(float* s);
	void scale(const rsVec &vec);
	void rotate(float a, float x, float y, float z);  // normalized angle, axis
	void rotate(float a, const rsVec &v);  // normalized angle, axis
	void rotate(rsQuat &q);
	float determinant3(const float aa, const float ab, const float ac,
		const float ba, const float bb, const float bc,
		const float ca, const float cb, const float cc);
	bool invert();  // general matrix inversion
	bool invert(const rsMatrix &mat);  // general matrix inversion
	void rotationInvert(const rsMatrix &mat);  // rotation matrix inversion
	void fromQuat(const rsQuat &q);

	float & operator [] (int i)
	{
		return m[i];
	}

	const float & operator [] (int i) const
	{
		return m[i];
	}

	rsMatrix & operator = (const rsMatrix &mat);
	std::ostream & operator << (std::ostream &os);
	//	friend std::ostream & operator << (std::ostream& os, const rsMatrix& mat);
};

#endif
