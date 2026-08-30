/*
 * Copyright (C) 2005-2010  Terence M. Welsh
 *
 * This file is part of Hyperspace.
 *
 * Hyperspace is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Hyperspace is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#ifndef STRETCHED_PARTICLE_H
#define STRETCHED_PARTICLE_H



class stretchedParticle{
public:
	float pos[3];  // current position
	float lastPos[3];  // position at previous frame
	float drawPos[3];  // median position, where star is actually drawn
	double screenPos[2];  // point where pos maps to screen
	double lastScreenPos[2];  // point where lastPos maps to screen

	float fov;

	float radius;
	float color[3];

	stretchedParticle();
	~stretchedParticle(){};
	//void update();
	void draw(float* eyepoint);
};



#endif
