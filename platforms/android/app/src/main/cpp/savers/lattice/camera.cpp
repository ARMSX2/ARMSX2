/*
 * Copyright (C) 1999-2010  Terence M. Welsh
 *
 * This file is part of Lattice.
 *
 * Lattice is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Lattice is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include "camera.h"



void camera::init(float* mat, float f){
	//int i;
	float temp;

	// far clipping plane
	farplane = f;

	// bottom and top planes' vectors
	temp = atanf(1.0f / mat[5]);
	cullVec[0][0] = 0.0f;
	cullVec[0][1] = cosf(temp);
	cullVec[0][2] = -sinf(temp);
	cullVec[1][0] = 0.0f;
	cullVec[1][1] = -cullVec[0][1];
	cullVec[1][2] = cullVec[0][2];

	// left and right planes' vectors
	temp = atanf(1.0f / mat[0]);
	cullVec[2][0] = cosf(temp);
	cullVec[2][1] = 0.0f;
	cullVec[2][2] = -sinf(temp);
	cullVec[3][0] = -cullVec[2][0];
	cullVec[3][1] = 0.0f;
	cullVec[3][2] = cullVec[2][2];

	/*ofstream outf("outfile");
	outf << cullVec[0][0] << " " << cullVec[0][1] << " " << cullVec[0][2] << endl;
	outf << cullVec[1][0] << " " << cullVec[1][1] << " " << cullVec[1][2] << endl;
	outf << cullVec[2][0] << " " << cullVec[2][1] << " " << cullVec[2][2] << endl;
	outf << cullVec[3][0] << " " << cullVec[3][1] << " " << cullVec[3][2] << endl;
	outf.close();*/
}


bool camera::inViewVolume(float* pos, float radius){
	// check back plane
	if(pos[2] < -(farplane + radius))
		return 0;
	// check bottom plane
	else if(pos[0]*cullVec[0][0] + pos[1]*cullVec[0][1] + pos[2]*cullVec[0][2] < -radius)
		return 0;
	// check top plane
	else if(pos[0]*cullVec[1][0] + pos[1]*cullVec[1][1] + pos[2]*cullVec[1][2] < -radius)
		return 0;
	// check left plane
	else if(pos[0]*cullVec[2][0] + pos[1]*cullVec[2][1] + pos[2]*cullVec[2][2] < -radius)
		return 0;
	// check right plane
	else if(pos[0]*cullVec[3][0] + pos[1]*cullVec[3][1] + pos[2]*cullVec[3][2] < -radius)
		return 0;

	//static ofstream outf("outfile");
	//outf << pos[0] << " " << pos[1] << " " << pos[2] << endl;

	return 1;
}
