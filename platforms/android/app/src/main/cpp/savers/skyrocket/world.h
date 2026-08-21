/*
 * Copyright (C) 1999-2010  Terence M. Welsh
 *
 * This file is part of Skyrocket.
 *
 * Skyrocket is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Skyrocket is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#ifndef WORLD_H
#define WORLD_H



#define STARMESH 12
#define STARTEXSIZE 1024
#define MOONGLOWTEXSIZE 128
#define CLOUDMESH 70



extern int dWind;
extern int dAmbient;
extern int dStardensity;
extern int dMoonglow;
extern int dMoon;
extern int dClouds;
extern int dEarth;



class World{
public:
	int doSunset;
	float moonRotation, moonHeight;
	float cloudShift;
	float stars[STARMESH+1][STARMESH/2][6];  // 6 = x,y,z,u,v,bright
	float clouds[CLOUDMESH+1][CLOUDMESH+1][9];  // 9 = x,y,z,u,v,std bright,r,g,b
	unsigned int starlist;
	unsigned int startex;
	unsigned int moonlist;
	unsigned int moontex;
	unsigned int moonglowlist;
	unsigned int moonglowtex;
	unsigned int cloudtex;
	unsigned int sunsettex;
	unsigned int sunsetlist;
	unsigned int earthneartex;
	unsigned int earthfartex;
	unsigned int earthlighttex;
	unsigned int earthlist;
	unsigned int earthnearlist;
	unsigned int earthfarlist;

	World();
	~World(){}
	// For building mountain sillohettes in sunset
	void makeHeights(int first, int last, int *h);
	void update(float frameTime);
	void draw();
};



#endif
