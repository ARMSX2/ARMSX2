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


#include <GL/gl.h>
#include <math.h>

#include "shockwave.h"
#include "world.h"



extern World* theWorld;

#define WAVESTEPS 40
float shockwavegeom[7][WAVESTEPS+1][3];



void initShockwave(){
	int i, j;
	float ch, sh;

	shockwavegeom[0][0][0] = 1.0f;
	shockwavegeom[0][0][1] = 0.0f;
	shockwavegeom[0][0][2] = 0.0f;
	shockwavegeom[1][0][0] = 0.985f;
	shockwavegeom[1][0][1] = 0.035f;
	shockwavegeom[1][0][2] = 0.0f;
	shockwavegeom[2][0][0] = 0.95f;
	shockwavegeom[2][0][1] = 0.05f;
	shockwavegeom[2][0][2] = 0.0f;
	shockwavegeom[3][0][0] = 0.85f;
	shockwavegeom[3][0][1] = 0.05f;
	shockwavegeom[3][0][2] = 0.0f;
	shockwavegeom[4][0][0] = 0.75f;
	shockwavegeom[4][0][1] = 0.035f;
	shockwavegeom[4][0][2] = 0.0f;
	shockwavegeom[5][0][0] = 0.65f;
	shockwavegeom[5][0][1] = 0.01f;
	shockwavegeom[5][0][2] = 0.0f;
	shockwavegeom[6][0][0] = 0.5f;
	shockwavegeom[6][0][1] = 0.0f;
	shockwavegeom[6][0][2] = 0.0f;

	for(i=1; i<=WAVESTEPS; i++){
		ch = cosf(6.28318530718f * (float(i) / float(WAVESTEPS)));
		sh = sinf(6.28318530718f * (float(i) / float(WAVESTEPS)));
		for(j=0; j<=6; j++){
			shockwavegeom[j][i][0] = ch * shockwavegeom[j][0][0];
			shockwavegeom[j][i][1] = shockwavegeom[j][0][1];
			shockwavegeom[j][i][2] = sh * shockwavegeom[j][0][0];
		}
	}
}

// temp influences color intensity (0.0 - 1.0)
// texmove is amount to advance the texture coordinates
void drawShockwave(float temperature, float texmove){
	static int i, j;
	float colors[7][4];
	static float u, v1, v2;
	float temp;

	// setup diminishing alpha values in color array
	temp = temperature * temperature;
	colors[0][3] = temp;
	colors[1][3] = temp * 0.9f;
	colors[2][3] = temp * 0.8f;
	colors[3][3] = temp * 0.7f;
	colors[4][3] = temp * 0.5f;
	colors[5][3] = temp * 0.3f;
	colors[6][3] = 0.0f;
	// setup rgb values in color array
	for(i=0; i<=5; i++){
		colors[i][0] = 1.0f;
		colors[i][1] = (temperature + 1.0f) * 0.5f;
		colors[i][2] = temperature;
	}

	glDisable(GL_CULL_FACE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glEnable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, theWorld->cloudtex);

	// draw bottom of shockwave
	for(i=0; i<6; i++){
		v1 = float(i+1) * 0.07f - texmove;
		v2 = float(i) * 0.07f - texmove;
		glBegin(GL_TRIANGLE_STRIP);
		for(j=0; j<=WAVESTEPS; j++){
			u = (float(j) / float(WAVESTEPS)) * 10.0f;
			glColor4fv(colors[i+1]);
			glTexCoord2f(u, v1);
			glVertex3f(shockwavegeom[i+1][j][0], -shockwavegeom[i+1][j][1], shockwavegeom[i+1][j][2]);
			glColor4fv(colors[i]);
			glTexCoord2f(u, v2);
			glVertex3f(shockwavegeom[i][j][0], -shockwavegeom[i][j][1], shockwavegeom[i][j][2]);
		}
		glEnd();
	}

	// keep colors a little warmer on top (more green)
	for(i=1; i<=6; i++)
		colors[i][1] = (temperature + 2.0f) * 0.333333f;
	
	// draw top of shockwave
	for(i=0; i<6; i++){
		v1 = float(i) * 0.07f - texmove;
		v2 = float(i+1) * 0.07f - texmove;
		glBegin(GL_TRIANGLE_STRIP);
		for(j=0; j<=WAVESTEPS; j++){
			u = (float(j) / float(WAVESTEPS)) * 10.0f;
			glColor4fv(colors[i]);
			glTexCoord2f(u, v1);
			glVertex3f(shockwavegeom[i][j][0], shockwavegeom[i][j][1], shockwavegeom[i][j][2]);
			glColor4fv(colors[i+1]);
			glTexCoord2f(u, v2);
			glVertex3f(shockwavegeom[i+1][j][0], shockwavegeom[i+1][j][1], shockwavegeom[i+1][j][2]);
		}
		glEnd();
	}

	glEnable(GL_CULL_FACE);
}
