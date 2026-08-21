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


#ifdef WIN32
#include <windows.h>
#include "extensions.h"
#endif

#include "starBurst.h"
#include "wavyNormalCubeMaps.h"
#include "flare.h"
#include <rsMath/rsMath.h>
#include <GL/gl.h>
#include <GL/glu.h>


extern int xsize, ysize;
extern float aspectRatio;
extern float frameTime;
extern float camPos[3];
extern int numAnimTexFrames;
extern wavyNormalCubeMaps* theWNCM;
extern unsigned int nebulatex;
extern unsigned int goo_vp, goo_fp;
extern int whichTexture;
extern float depth;
extern GLhandleARB gooProgram;
extern GLhandleARB tunnelProgram;


starBurst::starBurst(){
	int i, j;
	float vel[3];
	float normalizer;

	// initialize stars
	stars = new stretchedParticle*[SB_NUM_STARS];
	stars_active = new bool[SB_NUM_STARS];
	stars_velocity = new float*[SB_NUM_STARS];
	for(i=0; i<SB_NUM_STARS; i++){
		stars[i] = new stretchedParticle;
		stars_active[i] = 0;
		stars_velocity[i] = new float[3];
		vel[0] = rsRandf(1.0f) - 0.5f;
		vel[1] = rsRandf(1.0f) - 0.5f;
		vel[2] = rsRandf(1.0f) - 0.5f;
		normalizer = (rsRandf(0.75f) + 0.25f)
			/ sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
		stars_velocity[i][0] = vel[0] * normalizer;
		stars_velocity[i][1] = vel[1] * normalizer;
		stars_velocity[i][2] = vel[2] * normalizer;
	}

	float xyz[3];
	float ci, si, cj, sj, cjj, sjj;
	call_list = glGenLists(1);
	glNewList(call_list, GL_COMPILE);
		for(j=0; j<32; j++){
			cj = cosf(float(j) * RS_PIx2 / 32.0f);
			sj = sinf(float(j) * RS_PIx2 / 32.0f);
			cjj = cosf(float(j+1) * RS_PIx2 / 32.0f);
			sjj = sinf(float(j+1) * RS_PIx2 / 32.0f);
			glBegin(GL_TRIANGLE_STRIP);
				for(i=0; i<=32; i++){
					ci = cosf(float(i) * RS_PIx2 / 32.0f);
					si = sinf(float(i) * RS_PIx2 / 32.0f);
					xyz[0] = sj * ci;
					xyz[1] = cj;
					xyz[2] = sj * si;
					glNormal3fv(xyz);
					glVertex3fv(xyz);
					xyz[0] = sjj * ci;
					xyz[1] = cjj;
					xyz[2] = sjj * si;
					glNormal3fv(xyz);
					glVertex3fv(xyz);
				}
			glEnd();
		}
	glEndList();

	size = 4.0f;
}


void starBurst::restart(float* position){
	int i;

	for(i=0; i<SB_NUM_STARS; i++){  // don't restart if any star is still active
		if(stars_active[i])
			return;
	}
	if(size < 3.0f)  // or if flare hasn't faded out completely
		return;

	float color[3];
	color[0] = rsRandf(1.0f);
	color[1] = rsRandf(1.0f);
	color[2] = rsRandf(1.0f);
	color[rsRandi(3)] = 1.0f;
	for(i=0; i<SB_NUM_STARS; i++){
		stars_active[i] = 1;
		stars[i]->pos[0] = position[0];
		stars[i]->pos[1] = position[1];
		stars[i]->pos[2] = position[2];
		stars[i]->color[0] = color[0];
		stars[i]->color[1] = color[1];
		stars[i]->color[2] = color[2];
	}

	size = 0.0f;
	pos[0] = position[0];
	pos[1] = position[1];
	pos[2] = position[2];
}


void starBurst::drawStars(){
	int i;
	float distance;

	// draw stars
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, flaretex[0]);
	for(i=0; i<SB_NUM_STARS; i++){
		stars[i]->pos[0] += stars_velocity[i][0] * frameTime;
		stars[i]->pos[1] += stars_velocity[i][1] * frameTime;
		stars[i]->pos[2] += stars_velocity[i][2] * frameTime;
		distance = sqrtf((stars[i]->pos[0] - camPos[0]) * (stars[i]->pos[0] - camPos[0])
			+ (stars[i]->pos[1] - camPos[1]) * (stars[i]->pos[1] - camPos[1])
			+ (stars[i]->pos[2] - camPos[2]) * (stars[i]->pos[2] - camPos[2]));
		if(distance > depth)
			stars_active[i] = 0;
		if(stars_active[i])
			stars[i]->draw(camPos);
	}
}


void starBurst::draw(){
	drawStars();
	
	size += frameTime * 0.5f;
	if(size >= 3.0f)
		return;

	// draw flare
	float brightness = 1.0f - (size * 0.333333f);
	if(brightness > 0.0f){
		double p[3];
		p[0] = pos[0];
		p[1] = pos[1];
		p[2] = pos[2];
		flare(p, 1.0f, 1.0f, 1.0f, brightness);
	}

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glTranslatef(pos[0], pos[1], pos[2]);
	glScalef(size, size, size);

	// draw sphere
	glBindTexture(GL_TEXTURE_2D, nebulatex);
	glEnable(GL_TEXTURE_2D);
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
	glEnable(GL_TEXTURE_GEN_S);
	glEnable(GL_TEXTURE_GEN_T);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glEnable(GL_BLEND);
	glColor4f(brightness, brightness, brightness, 1.0f);
	glCallList(call_list);

	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);

	glPopMatrix();
}


void starBurst::draw(float lerp){
	drawStars();
	
	size += frameTime * 0.5f;
	if(size >= 3.0f)
		return;

	// draw flare
	float brightness = 1.0f - (size * 0.333333f);
	if(brightness > 0.0f){
		double p[3];
		p[0] = pos[0];
		p[1] = pos[1];
		p[2] = pos[2];
		flare(p, 1.0f, 1.0f, 1.0f, brightness);
	}

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glTranslatef(pos[0], pos[1], pos[2]);
	glScalef(size, size, size);

	// draw sphere
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_TEXTURE_CUBE_MAP_ARB);
	glActiveTextureARB(GL_TEXTURE2_ARB);
	glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, nebulatex);
	glActiveTextureARB(GL_TEXTURE1_ARB);
	glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, theWNCM->texture[(whichTexture + 1) % numAnimTexFrames]);
	glActiveTextureARB(GL_TEXTURE0_ARB);
	glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, theWNCM->texture[whichTexture]);
	glUseProgramObjectARB(gooProgram);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glEnable(GL_BLEND);
	glColor4f(brightness, brightness, brightness, lerp);
	glCallList(call_list);

	glDisable(GL_TEXTURE_CUBE_MAP_ARB);
	glUseProgramObjectARB(0);

	glPopMatrix();
}
