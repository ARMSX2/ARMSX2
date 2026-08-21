/*
 * Skyrocket, adapted for Android.
 *
 * NOT byte-identical to upstream: Skyrocket ships Windows-only in Terry Welsh's repo (no
 * RS_XSCREENSAVER path), so the Win32 pieces were removed by hand. A neutral version exists in
 * rss-glx, but that port is GPL-2.0-ONLY while this file is GPL-2.0-or-later, and only the
 * latter can also go into ARMSX2 (GPL-3.0).
 *
 * Removed:
 *   - <windows.h>, <regstr.h>, <commctrl.h>, rsWin32Saver, resource.h and the registry globals
 *   - the window/context setup at the top of initSaver(), which now takes the surface size from
 *     its caller; the host owns the EGL surface
 *   - wglSwapLayerBuffers(); the host view presents
 *   - everything after setDefaults(): registry read/write, config dialog, ScreenSaverProc
 *
 * Audio is off (dSound = 0): upstream uses OpenAL and bakes ~7MB of samples into headers.
 * soundEngine.h is a stub and the samples are not shipped. See that file.
 * The simulation and all drawing are untouched.
 */
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


// Skyrocket screen saver


#include <stdio.h>
#include <math.h>
#include <time.h>
#include <vector>

#include "gl1.h"

/* Mouse-button masks from windows.h. Skyrocket reads them in its interactive camera mode,
 * which a wallpaper never enters -- mouseButtons stays 0 -- but the expressions still have to
 * compile. */
#ifndef MK_LBUTTON
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_MBUTTON 0x0010
#endif


#include <rsMath/rsMath.h>
#include <rsText/rsText.h>

#include "particle.h"
#include "world.h"
//#include "overlay.h"



//#include <fstream>
//std::ofstream outfile;



// Global variables
int readyToDraw = 0;
// list of particles
//std::list<particle> particles;
// Time from one frame to the next
float frameTime = 0.0f;
// Window variables
int xsize, ysize, centerx, centery;
float aspectRatio;
float gWidth, gHeight;
float gFov, gHFov;
// Camera variables
static rsVec lookFrom[3];  // 3 = position, target position, last position
static rsVec lookAt[3]  // 3 = position, target position, last position
	= {rsVec(0.0f, 1000.0f, 0.0f), 
	rsVec(0.0f, 1000.0f, 0.0f), 
	rsVec(0.0f, 1000.0f, 0.0f)};
rsVec cameraPos;  // used for positioning sounds (same as lookFrom[0])
rsVec cameraVel;  // used for doppler shift
// Mouse variables
float mouseIdleTime;
int mouseButtons, mousex, mousey;
float mouseSpeed;

// the sound engine
SoundEngine* soundengine = NULL;

// flare display lists
unsigned int flarelist[4];
// matrix junk for drawing flares in screen space
double modelMat[16], projMat[16];
int viewport[4];

// transformation needed for rendering particles
float billboardMat[16];

// lifespans for smoke particles
float smokeTime[SMOKETIMES];  // lifespans of consecutive smoke particles
int whichSmoke[WHICHSMOKES];  // table to indicate which particles produce smoke
// smoke display lists
unsigned int smokelist[5];

// the world
World* theWorld;

// text output
rsText* textwriter;


int numRockets = 0;
std::vector<flareData> lensFlares;
int numFlares = 0;
// Parameters edited in the dialog box
int dMaxrockets;
int dSmoke;
int dExplosionsmoke;
int dWind;
int dAmbient;
int dStardensity;
int dFlare;
int dMoonglow;
int dMoon;
int dClouds;
int dEarth;
int dIllumination;
int dSound;
// Commands given from keyboard
int kFireworks = 1;
int kCamera = 1;  // 0 = paused, 1 = autonomous, 2 = mouse control
int kNewCamera = 0;
bool kSlowMotion = false;
int userDefinedExplosion = -1;



std::vector<particle> particles;
unsigned int last_particle = 0;
#define ZOOMROCKETINACTIVE 1000000000
unsigned int zoomRocket = ZOOMROCKETINACTIVE;



particle* addParticle(){
	// Advance to new particle if there is another in the vector.
	// Otherwise, just overwrite the last particle (this will probably never happen)
	if(last_particle < particles.size())
		++last_particle;

	// Return pointer to new particle
	return &(particles[last_particle-1]);
}


void removeParticle(unsigned int rempart){
	// copy last particle over particle to be removed
	--last_particle;
	if(rempart != last_particle)
		particles[rempart] = particles[last_particle];

	// correct zoomRocket index if necessary
	if(zoomRocket == last_particle)
		zoomRocket = rempart;
}


void sortParticles(){
	// Sorting doesn't appear to be necessary.  Skyrocket still looks good without it.
}


// Rockets and explosions illuminate smoke
// Only explosions illuminate clouds
void illuminate(particle* ill){
	float temp;
	// desaturate illumination colors
	rsVec newrgb(ill->rgb[0] * 0.6f + 0.4f, ill->rgb[1] * 0.6f + 0.4f, ill->rgb[2] * 0.6f + 0.4f);

	// Smoke illumination
	if((ill->type == ROCKET) || (ill->type == FOUNTAIN)){
		float distsquared;
		for(unsigned int i=0; i<last_particle; ++i){
			particle* smk(&(particles[i]));
			if(smk->type == SMOKE){
				distsquared = (ill->xyz[0] - smk->xyz[0]) * (ill->xyz[0] - smk->xyz[0])
					+ (ill->xyz[1] - smk->xyz[1]) * (ill->xyz[1] - smk->xyz[1])
					+ (ill->xyz[2] - smk->xyz[2]) * (ill->xyz[2] - smk->xyz[2]);
				if(distsquared < 40000.0f){
					temp = (40000.0f - distsquared) * 0.000025f;
					temp = temp * temp * ill->bright;
					smk->rgb[0] += temp * newrgb[0];
					if(smk->rgb[0] > 1.0f)
						smk->rgb[0] = 1.0f;
					smk->rgb[1] += temp * newrgb[1];
					if(smk->rgb[1] > 1.0f)
						smk->rgb[1] = 1.0f;
					smk->rgb[2] += temp * newrgb[2];
					if(smk->rgb[2] > 1.0f)
						smk->rgb[2] = 1.0f;
				}
			}
		}
	}
	if(ill->type == EXPLOSION){
		float distsquared;
		for(unsigned int i=0; i<last_particle; ++i){
			particle* smk(&(particles[i]));
			if(smk->type == SMOKE){
				distsquared = (ill->xyz[0] - smk->xyz[0]) * (ill->xyz[0] - smk->xyz[0])
					+ (ill->xyz[1] - smk->xyz[1]) * (ill->xyz[1] - smk->xyz[1])
					+ (ill->xyz[2] - smk->xyz[2]) * (ill->xyz[2] - smk->xyz[2]);
				if(distsquared < 640000.0f){
					temp = (640000.0f - distsquared) * 0.0000015625f;
					temp = temp * temp * ill->bright;
					smk->rgb[0] += temp * newrgb[0];
					if(smk->rgb[0] > 1.0f)
						smk->rgb[0] = 1.0f;
					smk->rgb[1] += temp * newrgb[1];
					if(smk->rgb[1] > 1.0f)
						smk->rgb[1] = 1.0f;
					smk->rgb[2] += temp * newrgb[2];
					if(smk->rgb[2] > 1.0f)
						smk->rgb[2] = 1.0f;
				}
			}
		}
	}

	// cloud illumination
	if(ill->type == EXPLOSION && dClouds){
		int north, south, west, east;  // limits of cloud indices to inspect
		int halfmesh = CLOUDMESH / 2;
		float distsquared;
		// remember clouds have 20000-foot radius from the World class, hence 0.00005
		// Hardcoded values like this are evil, but oh well
		south = int((ill->xyz[2] - 1600.0f) * 0.00005f * float(halfmesh)) + halfmesh;
		north = int((ill->xyz[2] + 1600.0f) * 0.00005f * float(halfmesh) + 0.5f) + halfmesh;
		west = int((ill->xyz[0] - 1600.0f) * 0.00005f * float(halfmesh)) + halfmesh;
		east = int((ill->xyz[0] + 1600.0f) * 0.00005f * float(halfmesh) + 0.5f) + halfmesh;
		// bound these values just in case
		if(south < 0) south = 0; if(south > CLOUDMESH-1) south = CLOUDMESH-1;
		if(north < 0) north = 0; if(north > CLOUDMESH-1) north = CLOUDMESH-1;
		if(west < 0) west = 0; if(west > CLOUDMESH-1) west = CLOUDMESH-1;
		if(east < 0) east = 0; if(east > CLOUDMESH-1) east = CLOUDMESH-1;
		//do any necessary cloud illumination
		for(int i=west; i<=east; i++){
			for(int j=south; j<=north; j++){
				distsquared = (theWorld->clouds[i][j][0] - ill->xyz[0]) * (theWorld->clouds[i][j][0] - ill->xyz[0])
					+ (theWorld->clouds[i][j][1] - ill->xyz[1]) * (theWorld->clouds[i][j][1] - ill->xyz[1])
					+ (theWorld->clouds[i][j][2] - ill->xyz[2]) * (theWorld->clouds[i][j][2] - ill->xyz[2]);
				if(distsquared < 2560000.0f){
					temp = (2560000.0f - distsquared) * 0.000000390625f;
					temp = temp * temp * ill->bright;
					theWorld->clouds[i][j][6] += temp * newrgb[0];
					if(theWorld->clouds[i][j][6] > 1.0f)
						theWorld->clouds[i][j][6] = 1.0f;
					theWorld->clouds[i][j][7] += temp * newrgb[1];
					if(theWorld->clouds[i][j][7] > 1.0f)
						theWorld->clouds[i][j][7] = 1.0f;
					theWorld->clouds[i][j][8] += temp * newrgb[2];
					if(theWorld->clouds[i][j][8] > 1.0f)
						theWorld->clouds[i][j][8] = 1.0f;
				}
			}
		}
	}
}


// pulling of other particles
void pulling(particle* suck){
	rsVec diff;
	float pulldistsquared;
	float pullconst = (1.0f - suck->life) * 0.01f * frameTime;

	for(unsigned int i=0; i<last_particle; ++i){
		particle* puller(&(particles[i]));
		diff = suck->xyz - puller->xyz;
		pulldistsquared = diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2];
		if(pulldistsquared < 250000.0f && pulldistsquared != 0.0f){
			if(puller->type != SUCKER && puller->type != STRETCHER
				&& puller->type != SHOCKWAVE && puller->type != BIGMAMA){
				diff.normalize();
				puller->vel += diff * ((250000.0f - pulldistsquared) * pullconst);
			}
		}
	}
}


// pushing of other particles
void pushing(particle* shock){
	rsVec diff;
	float pushdistsquared;
	float pushconst = (1.0f - shock->life) * 0.002f * frameTime;

	for(unsigned int i=0; i<last_particle; ++i){
		particle* pusher(&(particles[i]));
		diff = pusher->xyz - shock->xyz;
		pushdistsquared = diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2];
		if(pushdistsquared < 640000.0f && pushdistsquared != 0.0f){
			if(pusher->type != SUCKER && pusher->type != STRETCHER
				&& pusher->type != SHOCKWAVE && pusher->type != BIGMAMA){
				diff.normalize();
				pusher->vel += diff * ((640000.0f - pushdistsquared) * pushconst);
			}
		}
	}
}


// vertical stretching of other particles (x, z sucking; y pushing)
void stretching(particle* stretch){
	rsVec diff;
	float stretchdistsquared, temp;
	float stretchconst = (1.0f - stretch->life) * 0.002f * frameTime;

	for(unsigned int i=0; i<last_particle; ++i){
		particle* stretcher(&(particles[i]));
		diff = stretch->xyz - stretcher->xyz;
		stretchdistsquared = diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2];
		if(stretchdistsquared < 640000.0f && stretchdistsquared != 0.0f && stretcher->type != STRETCHER){
			diff.normalize();
			temp = (640000.0f - stretchdistsquared) * stretchconst;
			stretcher->vel[0] += diff[0] * temp * 5.0f;
			stretcher->vel[1] -= diff[1] * temp;
			stretcher->vel[2] += diff[2] * temp * 5.0f;
		}
	}
}


// Makes list of lens flares.  Must be a called even when action is paused
// because camera might still be moving.
void makeFlareList(){
	rsVec cameraDir, partDir;
	const float shine(float(dFlare) * 0.01f);

	cameraDir = lookAt[0] - lookFrom[0];
	cameraDir.normalize();
	for(unsigned int i=0; i<last_particle; ++i){
		particle* curlight(&(particles[i]));
		if(curlight->type == EXPLOSION || curlight->type == SUCKER
			|| curlight->type == SHOCKWAVE || curlight->type == STRETCHER
			|| curlight->type == BIGMAMA){
			double winx, winy, winz;
			gluProject(curlight->xyz[0], curlight->xyz[1], curlight->xyz[2],
				modelMat, projMat, viewport,
				&winx, &winy, &winz);
			partDir = curlight->xyz - cameraPos;
			if(partDir.dot(cameraDir) > 1.0f){  // is light source in front of camera?
				if(numFlares == lensFlares.size())
					lensFlares.resize(lensFlares.size() + 10);
				lensFlares[numFlares].x = (float(winx) / float(xsize)) * aspectRatio;
				lensFlares[numFlares].y = float(winy) / float(ysize);
				rsVec vec = curlight->xyz - cameraPos;  // find distance attenuation factor
				if(curlight->type == EXPLOSION){
					lensFlares[numFlares].r = curlight->rgb[0];
					lensFlares[numFlares].g = curlight->rgb[1];
					lensFlares[numFlares].b = curlight->rgb[2];
					float distatten = (10000.0f - vec.length()) * 0.0001f;
					if(distatten < 0.0f)
						distatten = 0.0f;
					lensFlares[numFlares].a = curlight->bright * shine * distatten;
				}
				else{
					lensFlares[numFlares].r = 1.0f;
					lensFlares[numFlares].g = 1.0f;
					lensFlares[numFlares].b = 1.0f;
					float distatten = (20000.0f - vec.length()) * 0.00005f;
					if(distatten < 0.0f)
						distatten = 0.0f;
					lensFlares[numFlares].a = curlight->bright * 2.0f * shine * distatten;
				}
				numFlares++;
			}
		}
	}
}


void randomLookFrom(int n){
	lookFrom[n][0] = rsRandf(6000.0f) - 3000.0f;
	lookFrom[n][1] = rsRandf(1200.0f) + 5.0f;
	lookFrom[n][2] = rsRandf(6000.0f) - 3000.0f;
}


void randomLookAt(int n){
	// look left or right some amount within HFov.  This way, if there is a really
	// wide FOV due to a wide screen or multiple monitors, the action will appear off
	// to the sides sometimes.
	float shift_angle = (gHFov * 0.5f) - 15.0f;
	if(shift_angle < 0.0f)
		shift_angle = 0.0f;
	const float shift = tanf(shift_angle / RS_RAD2DEG);
	const float shift_x = -lookFrom[n][2] * shift;
	const float shift_z = lookFrom[n][0] * shift;
	lookAt[n][0] = rsRandf(shift_x * 2.0f) - shift_x;
	lookAt[n][1] = rsRandf(800.0f) + 200.0f;
	lookAt[n][2] = rsRandf(shift_z * 2.0f) - shift_z;
}


void findHeadingAndPitch(rsVec lookFrom, rsVec lookAt, float& heading, float& pitch){
	const float diffx(lookAt[0] - lookFrom[0]);
	const float diffy(lookAt[1] - lookFrom[1]);
	const float diffz(lookAt[2] - lookFrom[2]);
	const float radius(sqrtf(diffx * diffx + diffz * diffz));
	pitch = R2D * atan2f(diffy, radius);
	heading = R2D * atan2f(-diffx, -diffz);
}


void reshape(){
	// build viewing matrix
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	if(aspectRatio > 1.0f){
		gHFov = 2.0f * RS_RAD2DEG * atanf(tanf(gFov * 0.5f / RS_RAD2DEG) * aspectRatio);
		gluPerspective(gFov, aspectRatio, 1.0f, 40000.0f);
	}
	else{
		gHFov = gFov;
		gluPerspective(2.0f * RS_RAD2DEG * atanf(tanf(gFov * 0.5f / RS_RAD2DEG) / aspectRatio), aspectRatio, 1.0f, 40000.0f);
	}
	glGetDoublev(GL_PROJECTION_MATRIX, projMat);
}


void draw(){
	static float cameraAngle = 0.0f;
	static const float firstHeading = rsRandf(2.0f * PIx2);
	static const float firstRadius = rsRandf(2000.0f);
	static int lastCameraMode = kCamera;
	static float cameraTime[3]  // time, elapsed time, step (1.0 - 0.0)
		= {20.0f, 0.0f, 0.0f};
	static float zoom = 0.0f;  // For interpolating from regular camera view to zoomed in view
	static float zoomTime[2] = {300.0f, 0.0f};  // time until next zoom, duration of zoom
	static float heading, pitch;
	static float zoomHeading = 0.0f;
	static float zoomPitch = 0.0f;

	// Variables for printing text
	static float computeTime = 0.0f;
	static float drawTime = 0.0f;
	static rsTimer computeTimer, drawTimer;
	// start compute time timer
	computeTimer.tick();

	// super fast easter egg
	static int superFast = rsRandi(1000);
	if(!superFast)
		frameTime *= 5.0f;

	////////////////////////////////
	// update camera
	///////////////////////////////
	static int first = 1;
	if(first){
		randomLookFrom(1);  // new target position
		randomLookAt(1);
		// starting camera view is very far away
		lookFrom[2] = rsVec(rsRandf(1000.0f) + 6000.0f, 5.0f, rsRandf(4000.0f) - 2000.0f);
		randomLookAt(2);
		textwriter = new rsText;
		first = 0;
	}

	// Make new random camera view
	if(kNewCamera){
		cameraTime[0] = rsRandf(25.0f) + 5.0f;
		cameraTime[1] = 0.0f;
		cameraTime[2] = 0.0f;
		// choose new positions
		randomLookFrom(1);  // new target position
		randomLookAt(1);  // new target position
		// cut to a new view
		randomLookFrom(2);  // new last position
		randomLookAt(2);  // new last position
		kNewCamera = 0;
	}

	// Update the camera if it is active
	if(kCamera == 1){
		if(lastCameraMode == 2){  // camera was controlled by mouse last frame
			cameraTime[0] = 10.0f;
			cameraTime[1] = 0.0f;
			cameraTime[2] = 0.0f;
			lookFrom[2] = lookFrom[0];
			randomLookFrom(1);  // new target position
			lookAt[2] = lookAt[0];
			randomLookAt(1);  // new target position
		}
		cameraTime[1] += frameTime;
		cameraTime[2] = cameraTime[1] / cameraTime[0];
		if(cameraTime[2] >= 1.0f){  // reset camera sequence
			// reset timer
			cameraTime[0] = rsRandf(25.0f) + 5.0f;
			cameraTime[1] = 0.0f;
			cameraTime[2] = 0.0f;
			// choose new positions
			lookFrom[2] = lookFrom[1];  // last = target
			randomLookFrom(1);  // new target position
			lookAt[2] = lookAt[1];  // last = target
			randomLookAt(1);  // new target position
			if(!rsRandi(4) && zoom == 0.0f){  // possibly cut to new view if camera isn't zoomed in
				randomLookFrom(2);  // new last position
				randomLookAt(2);
			}
		}
		// change camera position and angle
		float cameraStep = 0.5f * (1.0f - cosf(cameraTime[2] * PI));
		lookFrom[0] = lookFrom[2] + ((lookFrom[1] - lookFrom[2]) * cameraStep);
		lookAt[0] = lookAt[2] + ((lookAt[1] - lookAt[2]) * cameraStep);
		// update variables used for sound and lens flares
		cameraVel = lookFrom[0] - cameraPos;
		cameraPos = lookFrom[0];
		// find heading and pitch
		findHeadingAndPitch(lookFrom[0], lookAt[0], heading, pitch);

		// zoom in on rockets with camera
		zoomTime[0] -= frameTime;
		if(zoomTime[0] < 0.0f){
			if(zoomRocket == ZOOMROCKETINACTIVE){  // try to find a rocket to follow
				for(unsigned int i=0; i<last_particle; ++i){
					if(particles[i].type == ROCKET){
						zoomRocket = i;
						if(particles[zoomRocket].tr > 4.0f){
							zoomTime[1] = particles[zoomRocket].tr;
							// get out of for loop if a suitable rocket has been found
							i = last_particle;
						}
						else
							zoomRocket = ZOOMROCKETINACTIVE;
					}
				}
				if(zoomRocket == ZOOMROCKETINACTIVE)
					zoomTime[0] = 5.0f;
			}
			if(zoomRocket != ZOOMROCKETINACTIVE){  // zoom in on this rocket
				zoom += frameTime * 0.5f;
				if(zoom > 1.0f)
					zoom = 1.0f;
				zoomTime[1] -= frameTime;
				float h, p;
				findHeadingAndPitch(lookFrom[0], particles[zoomRocket].xyz, h, p);
				// Don't wrap around
				while(h - heading < -180.0f)
					h += 360.0f;
				while(h - heading > 180.0f)
					h -= 360.0f;
				while(zoomHeading - h < -180.0f)
					zoomHeading += 360.0f;
				while(zoomHeading - h > 180.0f)
					zoomHeading -= 360.0f;
				// Make zoomed heading and pitch follow rocket closely but not exactly.
				// It would look weird because the rockets wobble sometimes.
				zoomHeading += (h - zoomHeading) * 10.0f * frameTime;
				zoomPitch += (p - zoomPitch) * 5.0f * frameTime;
				// End zooming
				if(zoomTime[1] < 0.0f){
					zoomRocket = ZOOMROCKETINACTIVE;
					// Zoom in again no later than 3 minutes from now
					zoomTime[0] = rsRandf(175.0f) + 5.0f;
				}
			}
		}
	}

	// Still counting down to zoom in on a rocket,
	// so keep zoomed out.
	if(zoomTime[0] > 0.0f){
		zoom -= frameTime * 0.5f;
		if(zoom < 0.0f)
			zoom = 0.0f;
	}

	// Control camera with the mouse
	if(kCamera == 2){
		// find heading and pitch to compute rotation component of modelview matrix
		heading += 100.0f * frameTime * aspectRatio * float(centerx - mousex) / float(xsize);
		pitch += 100.0f * frameTime * float(centery - mousey) / float(ysize);
		if(heading > 180.0f)
			heading -= 360.0f;
		if(heading < -180.0f)
			heading += 360.0f;
		if(pitch > 90.0f)
			pitch = 90.0f;
		if(pitch < -90.0f)
			pitch = -90.0f;
		if(mouseButtons & MK_LBUTTON)
			mouseSpeed += 400.0f * frameTime;
		if(mouseButtons & MK_RBUTTON)
			mouseSpeed -= 400.0f * frameTime;
		if((mouseButtons & MK_MBUTTON) || ((mouseButtons & MK_LBUTTON) && (mouseButtons & MK_RBUTTON)))
			mouseSpeed = 0.0f;
		if(mouseSpeed > 4000.0f)
			mouseSpeed = 4000.0f;
		if(mouseSpeed < -4000.0f)
			mouseSpeed = -4000.0f;
		// find lookFrom location to compute translation component of modelview matrix
		float ch = cosf(D2R * heading);
		float sh = sinf(D2R * heading);
		float cp = cosf(D2R * pitch);
		float sp = sinf(D2R * pitch);
		lookFrom[0][0] -= mouseSpeed * sh * cp * frameTime;
		lookFrom[0][1] += mouseSpeed * sp * frameTime;
		lookFrom[0][2] -= mouseSpeed * ch * cp * frameTime;
		cameraPos = lookFrom[0];
		// Calculate new lookAt position so that lens flares will be computed correctly
		// and so that transition back to autonomous camera mode is smooth
		lookAt[0][0] = lookFrom[0][0] - 500.0f * sh * cp;
		lookAt[0][1] = lookFrom[0][1] + 500.0f * sp;
		lookAt[0][2] = lookFrom[0][2] - 500.0f * ch * cp;
	}

	// Interpolate fov, heading, and pitch using zoom value
	// zoom of {0,1} maps to fov of {60,6}
	const float t(0.5f * (1.0f - cosf(RS_PI * zoom)));
	gFov = 60.0f - 54.0f * t;
	heading = zoomHeading * t + heading * (1.0f - t);
	pitch = zoomPitch * t + pitch * (1.0f - t);

	reshape();

	// Build modelview matrix
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRotatef(-pitch, 1, 0, 0);
	glRotatef(-heading, 0, 1, 0);
	glTranslatef(-lookFrom[0][0], -lookFrom[0][1], -lookFrom[0][2]);
	// get modelview matrix for flares
	glGetDoublev(GL_MODELVIEW_MATRIX, modelMat);

	// store this frame's camera mode for next frame
	lastCameraMode = kCamera;
	// Update mouse idle time
	if(kCamera == 2){
		mouseIdleTime += frameTime;
		if(mouseIdleTime > 300.0f)  // return to autonomous camera mode after 5 minutes
			kCamera = 1;
	}

	// update billboard rotation matrix for particles
	glPushMatrix();
	glLoadIdentity();
	glRotatef(heading, 0, 1, 0);
	glRotatef(pitch, 1, 0, 0);
	glGetFloatv(GL_MODELVIEW_MATRIX, billboardMat);
	glPopMatrix();

	// clear the screen
	glClear(GL_COLOR_BUFFER_BIT);

	// Slows fireworks, but not camera
	if(kSlowMotion)
		frameTime *= 0.5f;

	// Make more particles if necessary (always keep 1000 extra).
	// Ordinarily, you would resize as needed during the update loop, probably in the
	// addParticle() function.  But that logic doesn't work with this particle system
	// because particles can spawn other particles.  resizing the vector, and, thus, 
	// moving all particle addresses, doesn't work if you are in the middle of
	// updating a particle.
	const unsigned int size(particles.size());
	if(particles.size() - int(last_particle) < 1000)
		particles.resize(particles.size() + 1000);

	// Pause the animation?
	if(kFireworks){
		// update world
		theWorld->update(frameTime);
	
		// darken smoke
		static float ambientlight = float(dAmbient) * 0.01f;
		for(unsigned int i=0; i<last_particle; ++i){
			particle* darkener(&(particles[i]));
			if(darkener->type == SMOKE)
				darkener->rgb[0] = darkener->rgb[1] = darkener->rgb[2] = ambientlight;
		}

		// Change rocket firing rate
		static float rocketTimer = 0.0f;
		// a rocket usually lasts about 10 seconds, so fastest rate is all rockets within 10 seconds
		static float rocketTimeConst = 10.0f / float(dMaxrockets);
		static float changeRocketTimeConst = 20.0f;
		changeRocketTimeConst -= frameTime;
		if(changeRocketTimeConst <= 0.0f){
			float temp;
			if(rsRandi(10))
				temp = rsRandf(4.0f);
			else
				temp = 0.0f;
			rocketTimeConst = (temp * temp) + (10.0f / float(dMaxrockets));
			changeRocketTimeConst = rsRandf(30.0f) + 10.0f;
		}
		// add new rocket to list
		rocketTimer -= frameTime;
		if((rocketTimer <= 0.0f) || (userDefinedExplosion >= 0)){
			if(numRockets < dMaxrockets){
				particle* rock = addParticle();
				if(rsRandi(30) || (userDefinedExplosion >= 0)){  // Usually launch a rocket
					rock->initRocket();
					if(userDefinedExplosion >= 0)
						rock->explosiontype = userDefinedExplosion;
					else{
						if(!rsRandi(2500)){  // big ones!
							if(rsRandi(2))
								rock->explosiontype = 19;  // sucker and shockwave
							else
								rock->explosiontype = 20;  // stretcher and bigmama
						}
						else{
							// Distribution of regular explosions
							if(rsRandi(2)){  // 0 - 2 (all types of spheres)
								if(!rsRandi(10))
									rock->explosiontype = 2;
								else
									rock->explosiontype = rsRandi(2);
							}
							else{
								if(!rsRandi(3))  //  ring, double sphere, sphere and ring
									rock->explosiontype = rsRandi(3) + 3;
								else{
									if(rsRandi(2)){  // 6, 7, 8, 9, 10, 11
										if(rsRandi(2))
											rock->explosiontype = rsRandi(2) + 6;
										else
											rock->explosiontype = rsRandi(4) + 8;
									}
									else{
										if(rsRandi(2))  // 12, 13, 14
											rock->explosiontype = rsRandi(3) + 12;
										else  // 15 - 18
											rock->explosiontype = rsRandi(4) + 15;
									}
								}
							}
						}
					}
					numRockets++;
				}
				else{  // sometimes make fountains instead of rockets
					rock->initFountain();
					int num_fountains = rsRandi(3);
					for(int i=0; i<num_fountains; i++){
						rock = addParticle();
						rock->initFountain();
					}
				}
			}
			if(dMaxrockets)
				rocketTimer = rsRandf(rocketTimeConst);
			else
				rocketTimer = 60.0f;  // arbitrary number since no rockets ever fire
			if(userDefinedExplosion >= 0){
				userDefinedExplosion = -1;
				rocketTimer = 20.0f;  // Wait 20 seconds after user launches a rocket before launching any more
			}
		}

		// update particles
		numRockets = 0;
		for(unsigned int i=0; i<last_particle; i++){
			particle* curpart(&(particles[i]));
			particles[i].update();
			if(curpart->type == ROCKET)
				numRockets++;
				curpart->findDepth();
			if(curpart->life <= 0.0f || curpart->xyz[1] < 0.0f){
				switch(curpart->type){
				case ROCKET:
					if(curpart->xyz[1] <= 0.0f){
						// move above ground for explosion so new particles aren't removed
						curpart->xyz[1] = 0.1f;
						curpart->vel[1] *= -0.7f;
					}
					if(curpart->explosiontype == 18)
						curpart->initSpinner();
					else
						curpart->initExplosion();
					break;
				case POPPER:
					switch(curpart->explosiontype){
					case STAR:
						curpart->explosiontype = 100;
						curpart->initExplosion();
						break;
					case STREAMER:
						curpart->explosiontype = 101;
						curpart->initExplosion();
						break;
					case METEOR:
						curpart->explosiontype = 102;
						curpart->initExplosion();
						break;
					case POPPER:
						curpart->type = STAR;
						curpart->rgb.set(1.0f, 0.8f, 0.6f);
						curpart->t = particles[i].tr = particles[i].life = 0.2f;
					}
					break;
				case SUCKER:
					curpart->initShockwave();
					break;
				case STRETCHER:
					curpart->initBigmama();
				}
			}
		}

		// remove particles from list
		for(unsigned int i=0; i<last_particle; i++){
			particle* curpart(&(particles[i]));
			if(curpart->life <= 0.0f || curpart->xyz[1] < 0.0f)
				removeParticle(i);
		}

		sortParticles();
	}  // kFireworks

	else{
		// Only sort particles if they're not being updated (the camera could still be moving)
		for(unsigned int i=0; i<last_particle; i++)
			particles[i].findDepth();
		sortParticles();
	}

	// measure compute time
	computeTime += float(computeTimer.tick());
	// start draw time timer
	drawTimer.tick();

	// the world
	theWorld->draw();

	// draw particles
	glEnable(GL_BLEND);
	for(unsigned int i=0; i<last_particle; i++)
		particles[i].draw();

	// draw lens flares
	if(dFlare){
		makeFlareList();
		for(int i=0; i<numFlares; ++i){
			flare(lensFlares[i].x, lensFlares[i].y, lensFlares[i].r,
				lensFlares[i].g, lensFlares[i].b, lensFlares[i].a);
		}
		numFlares = 0;
	}

	// measure draw time
	drawTime += float(drawTimer.tick());

	// do sound stuff
	if(soundengine){
		float listenerOri[6];
		listenerOri[0] = float(-modelMat[2]);
		listenerOri[1] = float(-modelMat[6]);
		listenerOri[2] = float(-modelMat[10]);
		listenerOri[3] = float(modelMat[1]);
		listenerOri[4] = float(modelMat[5]);
		listenerOri[5] = float(modelMat[9]);
		soundengine->update(cameraPos.v, cameraVel.v, listenerOri, frameTime, kSlowMotion);
	}

	//draw_overlay(frameTime);

	// print text
	static float totalTime = 0.0f;
	totalTime += frameTime;
	static std::vector<std::string> strvec;
	static int frames = 0;
	++frames;
	if(frames == 20){
		strvec.clear();
		std::string str1 = "         FPS = " + to_string(20.0f / totalTime);
		strvec.push_back(str1);
		std::string str2 = "compute time = " + to_string(computeTime / 20.0f);
		strvec.push_back(str2);
		std::string str3 = "   draw time = " + to_string(drawTime / 20.0f);
		strvec.push_back(str3);
		totalTime = 0.0f;
		computeTime = 0.0f;
		drawTime = 0.0f;
		frames = 0;
	}

	if(kStatistics){
		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();
		glOrtho(0.0f, 50.0f * aspectRatio, 0.0f, 50.0f, -1.0f, 1.0f);

		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();
		glTranslatef(1.0f, 48.0f, 0.0f);

		glColor3f(1.0f, 0.6f, 0.0f);
		textwriter->draw(strvec);

		glPopMatrix();
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
	}

}


void idleProc(){
	// update time
	static rsTimer timer;
	frameTime = float(timer.tick());

	if(readyToDraw && !isSuspended)
		draw();
}


void initSaver(int surfaceWidth, int surfaceHeight){
	// Initialize pseudorandom number generator
	srand((unsigned)time(NULL));

	// The host created the window and the GL context and passes the surface size in.
	xsize = surfaceWidth;
	ysize = surfaceHeight;
	centerx = xsize / 2;
	centery = ysize / 2;
	glViewport(0, 0, xsize, ysize);
	glGetIntegerv(GL_VIEWPORT, viewport);

	aspectRatio = float(xsize) / float(ysize);
	gFov = 60.0;
	reshape();

	// Set OpenGL state
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
	glFrontFace(GL_CCW);
	glEnable(GL_CULL_FACE);

	// Initialize data structures
	initFlares();
	//initRockets();
	if(dSmoke)
		initSmoke();
	theWorld = new World;
	initShockwave();
	if(dSound)
		soundengine = new SoundEngine(nullptr, float(dSound) * 0.01f);

	//init_overlay();

	//outfile.open("outfile");
}


void cleanup(){
	// Free memory
	particles.clear();

	// The counters that index into it have to go back with it. last_particle
	// left above zero against an emptied vector makes addParticle return
	// &particles[last_particle-1] on an empty vector, and the guard that would
	// have grown it again is unsigned arithmetic that wraps rather than going
	// negative.
	last_particle = 0;
	numRockets = 0;
	numFlares = 0;
	zoomRocket = ZOOMROCKETINACTIVE;

	// clean up sound data structures
	if(dSound)
		delete soundengine;

	// Upstream killed the device context here; the host owns it.
}


void setDefaults(){
	dMaxrockets = 8;
	dSmoke = 10;
	dExplosionsmoke = 0;
	dWind = 20;
	dAmbient = 5;
	dStardensity = 20;
	dFlare = 20;
	dMoonglow = 15;
	dSound = 100;
	dMoon = 1;
	dClouds = 1;
	dEarth = 1;
	dIllumination = 1;
}