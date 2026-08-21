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


#include "smoke.h"
#include "smoketex.h"



unsigned int smoketex[5];



// Initialize smoke texture objects and display lists
void initSmoke(){
	int i, j;

	unsigned char smoke1[SMOKETEXSIZE][SMOKETEXSIZE][2];
	unsigned char smoke2[SMOKETEXSIZE][SMOKETEXSIZE][2];
	unsigned char smoke3[SMOKETEXSIZE][SMOKETEXSIZE][2];
	unsigned char smoke4[SMOKETEXSIZE][SMOKETEXSIZE][2];
	unsigned char smoke5[SMOKETEXSIZE][SMOKETEXSIZE][2];

	for(i=0; i<SMOKETEXSIZE; i++){
		for(j=0; j<SMOKETEXSIZE; j++){
			smoke1[i][j][0] = 255;
			smoke2[i][j][0] = 255;
			smoke3[i][j][0] = 255;
			smoke4[i][j][0] = 255;
			smoke5[i][j][0] = 255;
			smoke1[i][j][1] = presmoke1[i][j];
			smoke2[i][j][1] = presmoke2[i][j];
			smoke3[i][j][1] = presmoke3[i][j];
			smoke4[i][j][1] = presmoke4[i][j];
			smoke5[i][j][1] = presmoke5[i][j];
		}
	}
	
	glGenTextures(5, smoketex);

	glBindTexture(GL_TEXTURE_2D, smoketex[0]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gluBuild2DMipmaps(GL_TEXTURE_2D, 2, SMOKETEXSIZE, SMOKETEXSIZE, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, smoke1);

	glBindTexture(GL_TEXTURE_2D, smoketex[1]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gluBuild2DMipmaps(GL_TEXTURE_2D, 2, SMOKETEXSIZE, SMOKETEXSIZE, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, smoke2);

	glBindTexture(GL_TEXTURE_2D, smoketex[2]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gluBuild2DMipmaps(GL_TEXTURE_2D, 2, SMOKETEXSIZE, SMOKETEXSIZE, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, smoke3);

	glBindTexture(GL_TEXTURE_2D, smoketex[3]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gluBuild2DMipmaps(GL_TEXTURE_2D, 2, SMOKETEXSIZE, SMOKETEXSIZE, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, smoke4);

	glBindTexture(GL_TEXTURE_2D, smoketex[4]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gluBuild2DMipmaps(GL_TEXTURE_2D, 2, SMOKETEXSIZE, SMOKETEXSIZE, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, smoke5);

	smokelist[0] = glGenLists(5);
	smokelist[1] = smokelist[0] + 1;
	smokelist[2] = smokelist[0] + 2;
	smokelist[3] = smokelist[0] + 3;
	smokelist[4] = smokelist[0] + 4;
	for(i=0; i<5; i++){
		glNewList(smokelist[i], GL_COMPILE);
			glBindTexture(GL_TEXTURE_2D, smoketex[i]);
			glBegin(GL_TRIANGLE_STRIP);
				glTexCoord2f(0.0f, 0.0f);
				glVertex3f(-0.5f, -0.5f, 0.0f);
				glTexCoord2f(1.0f, 0.0f);
				glVertex3f(0.5f, -0.5f, 0.0f);
				glTexCoord2f(0.0f, 1.0f);
				glVertex3f(-0.5f, 0.5f, 0.0f);
				glTexCoord2f(1.0f, 1.0f);
				glVertex3f(0.5f, 0.5f, 0.0f);
			glEnd();
		glEndList();
	}

	// set smoke lifespans  ( 1 2 1 4 1 2 1 8 )
	// This deserves a little more explanation:  smoke particles in this saver expand
	// over time.  If they all have the same lifespans, then they overlap too much and
	// that looks bad.  If every other particle fades out, the remaining ones have more
	// room to expand into.  So we use these smoke times to halve the number of particles
	// a few times.
	smokeTime[0] = smokeTime[2] = smokeTime[4] = smokeTime[6] = 0.4f;
	smokeTime[1] = smokeTime[5] = 0.8f;
	smokeTime[3] = 2.0f;
	smokeTime[7] = 4.0f;
	for(i=0; i<SMOKETIMES; i++){
		if(smokeTime[i] > float(dSmoke))
			smokeTime[i] = float(dSmoke);
	}
	if(smokeTime[7] < float(dSmoke))
		smokeTime[7] = float(dSmoke);

	// create table describing which particles will emit smoke
	// 0 = don't emit smoke
	// 1 = emit smoke
	for(i=0; i<WHICHSMOKES; i++)
		whichSmoke[i] = 0;
	if(dExplosionsmoke){
		float index = float(WHICHSMOKES) / float(dExplosionsmoke);
		for(i=0; i<dExplosionsmoke; i++)
			whichSmoke[int(float(i) * index)] = 1;
	}
}
