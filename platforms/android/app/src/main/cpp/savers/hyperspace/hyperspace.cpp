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


// Hyperspace screensaver
// Terry Welsh
// Originally wrote this saver in 2001, but computers weren't fast
// enough to run it at a decent frame rate.  Finally released it in 2005.

#ifdef WIN32
#include <windows.h>
#include <rsWin32Saver/rsWin32Saver.h>
#include <regstr.h>
#include <commctrl.h>
#include "resource.h"
#include "extensions.h"
#endif
#ifdef RS_XSCREENSAVER
#include <rsXScreenSaver/rsXScreenSaver.h>
#endif

#include <stdio.h>
#include <rsText/rsText.h>
#include <math.h>
#include <time.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <rsMath/rsMath.h>
#include "flare.h"
#include "causticTextures.h"
#include "wavyNormalCubeMaps.h"
#include "splinePath.h"
#include "tunnel.h"
#include "goo.h"
#include "stretchedParticle.h"
#include "starBurst.h"
#include "nebulamap.h"
#include "shaders.h"

//#include <fstream>
//std::ofstream outfile;


#ifdef WIN32
LPCTSTR registryPath = ("Software\\Really Slick\\Hyperspace");
HGLRC hglrc;
HDC hdc;
#endif
int readyToDraw = 0;
int xsize, ysize;
float aspectRatio;
float frameTime = 0.0f;
// text output
rsText* textwriter;
// Parameters edited in the dialog box
int dSpeed;
int dStars;
int dStarSize;
int dResolution;
int dDepth;
int dFov;
int dUseTunnels;
int dUseGoo;
int dShaders;


float unroll;
float billboardMat[16];
double modelMat[16];
double projMat[16];
int viewport[4];

float camPos[3] = {0.0f, 0.0f, 0.0f};
float depth;
int numAnimTexFrames = 20;
causticTextures* theCausticTextures;
wavyNormalCubeMaps* theWNCM; 
int whichTexture = 0;
splinePath* thePath;
tunnel* theTunnel;
goo* theGoo;
unsigned int speckletex, spheretex, nebulatex;
unsigned int goo_vp, goo_fp, tunnel_vp, tunnel_fp;

stretchedParticle** stars;
stretchedParticle* sunStar;
starBurst* theStarBurst;


void draw(){
	// Caustic textures and wavy normal cube maps are built on the first frame
	// rather than in initSaver, because they are rendered into the framebuffer
	// and read back, which needs a live context.
	//
	// The pointers are the record of whether that has happened. A separate
	// "already built" flag was tried and is worse in both directions: as a
	// static inside this function it survived cleanUp and the next frame drew
	// through freed pointers, and hoisted to file scope it was one more mutable
	// global of exactly the kind Task 6 is about. cleanUp nulls both, so these
	// guards restore themselves.
	//
	// No viewport restore here: causticTextures saves and restores it around
	// its own render (causticTextures.cpp:93, 219), and wavyNormalCubeMaps
	// never touches it.
	if(dUseTunnels && !theCausticTextures){
		glDisable(GL_FOG);
#ifdef WIN32
		if(doingPreview) // super fast for Windows previewer
			theCausticTextures = new causticTextures(8, numAnimTexFrames, 32, 32, 1.0f, 0.01f, 10.0f);
		else  // normal
#endif
			theCausticTextures = new causticTextures(8, numAnimTexFrames, 100, 256, 1.0f, 0.01f, 20.0f);
		glEnable(GL_FOG);
	}
	if(dShaders && !theWNCM){
#ifdef WIN32
		if(doingPreview) // super fast for Windows previewer
			theWNCM = new wavyNormalCubeMaps(numAnimTexFrames, 32);
		else  // normal
#endif
			theWNCM = new wavyNormalCubeMaps(numAnimTexFrames, 128);
	}

	// Variables for printing text
	static float computeTime = 0.0f;
	static float drawTime = 0.0f;
	static rsTimer computeTimer, drawTimer;
	// start compute time timer
	computeTimer.tick();

	glMatrixMode(GL_MODELVIEW);

	// Camera movements
	static float camHeading[3] = {0.0f, 0.0f, 0.0f};  // current, target, and last
	static int changeCamHeading = 1;
	static float camHeadingChangeTime[2] = {20.0f, 0.0f};  // total, elapsed
	static float camRoll[3] = {0.0f, 0.0f, 0.0f};  // current, target, and last
	static int changeCamRoll = 1;
	static float camRollChangeTime[2] = {1.0f, 0.0f};  // total, elapsed
	camHeadingChangeTime[1] += frameTime;
	if(camHeadingChangeTime[1] >= camHeadingChangeTime[0]){  // Choose new direction
		camHeadingChangeTime[0] = rsRandf(15.0f) + 5.0f;
		camHeadingChangeTime[1] = 0.0f;
		camHeading[2] = camHeading[1];  // last = target
		if(changeCamHeading){
			// face forward most of the time
			if(rsRandi(6))
				camHeading[1] = 0.0f;
			// face backward the rest of the time
			else if(rsRandi(2))
				camHeading[1] = RS_PI;
			else
				camHeading[1] = -RS_PI;
			changeCamHeading = 0;
		}
		else
			changeCamHeading = 1;
	}
	float t = camHeadingChangeTime[1] / camHeadingChangeTime[0];
	t = 0.5f * (1.0f - cosf(RS_PI * t));
	camHeading[0] = camHeading[1] * t + camHeading[2] * (1.0f - t);
	camRollChangeTime[1] += frameTime;
	if(camRollChangeTime[1] >= camRollChangeTime[0]){  // Choose new roll angle
		camRollChangeTime[0] = rsRandf(5.0f) + 10.0f;
		camRollChangeTime[1] = 0.0f;
		camRoll[2] = camRoll[1];  // last = target
		if(changeCamRoll){
			camRoll[1] = rsRandf(RS_PIx2*2) - RS_PIx2;
			changeCamRoll = 0;
		}
		else
			changeCamRoll = 1;
	}
	t = camRollChangeTime[1] / camRollChangeTime[0];
	t = 0.5f * (1.0f - cosf(RS_PI * t));
	camRoll[0] = camRoll[1] * t + camRoll[2] * (1.0f - t);

	static float pathDir[3] = {0.0f, 0.0f, -1.0f};
	thePath->moveAlongPath(float(dSpeed) * frameTime * 0.04f);
	thePath->update(frameTime);
	thePath->getPoint(dDepth + 2, thePath->step, camPos);
	thePath->getBaseDirection(dDepth + 2, thePath->step, pathDir);
	float pathAngle = atan2f(-pathDir[0], -pathDir[2]);

	glLoadIdentity();
	glRotatef((pathAngle + camHeading[0]) * RS_RAD2DEG, 0, 1, 0);
	glRotatef(camRoll[0] * RS_RAD2DEG, 0, 0, 1);
	glGetFloatv(GL_MODELVIEW_MATRIX, billboardMat);
	glLoadIdentity();
	glRotatef(-camRoll[0] * RS_RAD2DEG, 0, 0, 1);
	glRotatef((-pathAngle - camHeading[0]) * RS_RAD2DEG, 0, 1, 0);
	glTranslatef(-camPos[0], -camPos[1], -camPos[2]);
	glGetDoublev(GL_MODELVIEW_MATRIX, modelMat);
	unroll = camRoll[0] * RS_RAD2DEG;

	if(dUseGoo){
		// calculate diagonal fov
		float diagFov = 0.5f * float(dFov) / RS_RAD2DEG;
		diagFov = tanf(diagFov);
		diagFov = sqrtf(diagFov * diagFov + (diagFov * aspectRatio * diagFov * aspectRatio));
		diagFov = 2.0f * atanf(diagFov);
		theGoo->update(camPos[0], camPos[2], pathAngle + camHeading[0], diagFov);
	}

	// measure compute time
	computeTime += float(computeTimer.tick());
	// start draw time timer
	drawTimer.tick();

	// clear
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// draw stars
	glCullFace(GL_BACK);
	glEnable(GL_CULL_FACE);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glEnable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	// Units 1 and 2 only ever have anything bound to them by the shader path -
	// the goo's three-unit cube map below, the tunnel's second caustic frame,
	// and starBurst.cpp:225-230 - so with dShaders off there is nothing to
	// unbind and the entry point is null anyway: initExtensions resolves it
	// only when the extensions are present (extensions.cpp:76-91). The bind
	// below stays outside the guard because unit 0 is then never left.
	if(dShaders){
		glActiveTextureARB(GL_TEXTURE2_ARB);
		glBindTexture(GL_TEXTURE_2D, NULL);
		glActiveTextureARB(GL_TEXTURE1_ARB);
		glBindTexture(GL_TEXTURE_2D, NULL);
		glActiveTextureARB(GL_TEXTURE0_ARB);
	}
	glBindTexture(GL_TEXTURE_2D, flaretex[0]);
	static float temppos[2];
	for(int i=0; i<dStars; i++){
		temppos[0] = stars[i]->pos[0] - camPos[0];
		temppos[1] = stars[i]->pos[2] - camPos[2];
		if(temppos[0] > depth){
			stars[i]->pos[0] -= depth * 2.0f;
			stars[i]->lastPos[0] -= depth * 2.0f;
		}
		else if(temppos[0] < -depth){
			stars[i]->pos[0] += depth * 2.0f;
			stars[i]->lastPos[0] += depth * 2.0f;
		}
		if(temppos[1] > depth){
			stars[i]->pos[2] -= depth * 2.0f;
			stars[i]->lastPos[2] -= depth * 2.0f;
		}
		else if(temppos[1] < -depth){
			stars[i]->pos[2] += depth * 2.0f;
			stars[i]->lastPos[2] += depth * 2.0f;
		}
		stars[i]->draw(camPos);
	}
	glDisable(GL_CULL_FACE);

	// pick animated texture frame
	static float textureTime = 0.0f;
	textureTime += frameTime;
	// loop frames every 2 seconds
	const float texFrameTime(2.0f / float(numAnimTexFrames));
	while(textureTime > texFrameTime){
		textureTime -= texFrameTime;
		whichTexture ++;
	}
	while(whichTexture >= numAnimTexFrames)
		whichTexture -= numAnimTexFrames;

	// alpha component gets normalmap lerp value
	const float lerp = textureTime / texFrameTime;

	// draw goo
	if(dUseGoo){
		// calculate color
		static float goo_rgb_phase[3] = {-0.1f, -0.1f, -0.1f};
		static float goo_rgb_speed[3] = {rsRandf(0.02f) + 0.02f, rsRandf(0.02f) + 0.02f, rsRandf(0.02f) + 0.02f};
		float goo_rgb[4];
		for(int i=0; i<3; i++){
			goo_rgb_phase[i] += goo_rgb_speed[i] * frameTime;
			if(goo_rgb_phase[i] >= RS_PIx2)
				goo_rgb_phase[i] -= RS_PIx2;
			goo_rgb[i] = sinf(goo_rgb_phase[i]);
			if(goo_rgb[i] < 0.0f)
				goo_rgb[i] = 0.0f;
		}
		// setup textures
		if(dShaders){
			goo_rgb[3] = lerp;
			glDisable(GL_TEXTURE_2D);
			glEnable(GL_TEXTURE_CUBE_MAP_ARB);
			glActiveTextureARB(GL_TEXTURE2_ARB);
			glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, nebulatex);
			glActiveTextureARB(GL_TEXTURE1_ARB);
			glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, theWNCM->texture[(whichTexture + 1) % numAnimTexFrames]);
			glActiveTextureARB(GL_TEXTURE0_ARB);
			glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, theWNCM->texture[whichTexture]);
			glUseProgramObjectARB(gooProgram);
		}
		else{
			goo_rgb[3] = 1.0f;
			glBindTexture(GL_TEXTURE_2D, nebulatex);
			glEnable(GL_TEXTURE_2D);
			glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
			glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
			glEnable(GL_TEXTURE_GEN_S);
			glEnable(GL_TEXTURE_GEN_T);
		}
		// draw it
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glEnable(GL_BLEND);
		glColor4fv(goo_rgb);
		theGoo->draw();
		if(dShaders){
			glDisable(GL_TEXTURE_CUBE_MAP_ARB);
			glUseProgramObjectARB(0);
		}
		else{
			glDisable(GL_TEXTURE_GEN_S);
			glDisable(GL_TEXTURE_GEN_T);
		}
	}

	// update starburst
	static float starBurstTime = 300.0f;  // burst after 5 minutes
	starBurstTime -= frameTime;
	if(starBurstTime <= 0.0f){
		float pos[] = {camPos[0] + (pathDir[0] * depth * (0.5f + rsRandf(0.5f))),
			rsRandf(2.0f) - 1.0f,
			camPos[2] + (pathDir[2] * depth * (0.5f + rsRandf(0.5f)))};
		theStarBurst->restart(pos);  // it won't actually restart unless it's ready to
		starBurstTime = rsRandf(240.0f) + 60.0f;  // burst again within 1-5 minutes
	}
	if(dShaders)
		theStarBurst->draw(lerp);
	else
		theStarBurst->draw();

	// draw tunnel
	if(dUseTunnels){
		theTunnel->make(frameTime);
		glCullFace(GL_BACK);
		glEnable(GL_CULL_FACE);
		glEnable(GL_TEXTURE_2D);
		if(dShaders){
			glActiveTextureARB(GL_TEXTURE1_ARB);
			glBindTexture(GL_TEXTURE_2D, theCausticTextures->caustictex[(whichTexture + 1) % numAnimTexFrames]);
			glActiveTextureARB(GL_TEXTURE0_ARB);
			glBindTexture(GL_TEXTURE_2D, theCausticTextures->caustictex[whichTexture]);
			glUseProgramObjectARB(tunnelProgram);
			theTunnel->draw(lerp);
			glUseProgramObjectARB(0);
		}
		else{
			glBindTexture(GL_TEXTURE_2D, theCausticTextures->caustictex[whichTexture]);
			theTunnel->draw();
		}
		glDisable(GL_CULL_FACE);
	}

	// draw sun with lens flare
	glDisable(GL_FOG);
	double flarepos[3] = {0.0, 2.0, 0.0};
	glBindTexture(GL_TEXTURE_2D, flaretex[0]);
	sunStar->draw(camPos);
	float diff[3] = {float(flarepos[0] - camPos[0]), float(flarepos[1] - camPos[1]), float(flarepos[2] - camPos[2])};
	float alpha = 0.5f - 0.005f * sqrtf(diff[0] * diff[0] + diff[1] * diff[1] + diff[2] * diff[2]);
	if(alpha > 0.0f)
		flare(flarepos, 1.0f, 1.0f, 1.0f, alpha);
	glEnable(GL_FOG);

	// measure draw time
	drawTime += float(drawTimer.tick());

	// write text
	static float totalTime = 0.0f;
	totalTime += frameTime;
	static std::vector<std::string> strvec;
	static int frames = 0;
	++frames;
	if(frames == 60){
		strvec.clear();
		std::string str1 = "         FPS = " + to_string(60.0f / totalTime);
		strvec.push_back(str1);
		std::string str2 = "compute time = " + to_string(computeTime / 60.0f);
		strvec.push_back(str2);
		std::string str3 = "   draw time = " + to_string(drawTime / 60.0f);
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
	
#ifdef WIN32
	wglSwapLayerBuffers(hdc, WGL_SWAP_MAIN_PLANE);
#endif
#ifdef RS_XSCREENSAVER
	glXSwapBuffers(xdisplay, xwindow);
#endif
}


void idleProc(){
	// update time
	static rsTimer timer;
	frameTime = float(timer.tick());

#ifdef RS_XSCREENSAVER
	const bool shouldDraw = (readyToDraw && !isSuspended && !checkingPassword);
#else
	const bool shouldDraw = (readyToDraw && !isSuspended);
#endif
	if(shouldDraw)
		draw();
}


void setDefaults(){
	dSpeed = 10;
	dStars = 2000;
	dStarSize = 10;
	dResolution = 10;
	dDepth = 5;
	dFov = 50;
	dUseTunnels = 1;
	dUseGoo = 1;
	dShaders = 1;
	dFrameRateLimit = 0;
}


#ifdef RS_XSCREENSAVER
void handleCommandLine(int argc, char* argv[]){
	setDefaults();
	getArgumentsValue(argc, argv, std::string("-speed"), dSpeed, 1, 100);
	getArgumentsValue(argc, argv, std::string("-stars"), dStars, 0, 10000);
	getArgumentsValue(argc, argv, std::string("-starsize"), dStarSize, 1, 100);
	getArgumentsValue(argc, argv, std::string("-resolution"), dResolution, 4, 20);
	getArgumentsValue(argc, argv, std::string("-depth"), dDepth, 1, 10);
	getArgumentsValue(argc, argv, std::string("-fov"), dFov, 10, 150);
	getArgumentsValue(argc, argv, std::string("-usetunnels"), dUseTunnels, 0, 1);
	getArgumentsValue(argc, argv, std::string("-usegoo"), dUseGoo, 0, 1);
	getArgumentsValue(argc, argv, std::string("-shaders"), dShaders, 0, 1);
}
#endif


void reshape(int width, int height){
	glViewport(0, 0, width, height);
	aspectRatio = float(width) / float(height);

	xsize = width;
	ysize = height;

	viewport[0] = 0;
	viewport[1] = 0;
	viewport[2] = width;
	viewport[3] = height;

	// setup projection matrix
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(float(dFov), aspectRatio, 0.001f, 200.0f);
	glGetDoublev(GL_PROJECTION_MATRIX, projMat);
	glMatrixMode(GL_MODELVIEW);
}


#ifdef WIN32
void initSaver(HWND hwnd){
	RECT rect;

	// Window initialization
	hdc = GetDC(hwnd);
	setBestPixelFormat(hdc);
	hglrc = wglCreateContext(hdc);
	GetClientRect(hwnd, &rect);
	wglMakeCurrent(hdc, hglrc);

	// Disable vsync so frame rate is not capped at monitor refresh rate
	typedef BOOL (APIENTRY *PFNWGLSWAPINTERVALEXTPROC)(int);
	PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT =
		(PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
	if(wglSwapIntervalEXT)
		wglSwapIntervalEXT(0);

	// setup viewport
	//viewport[0] = rect.left;
	//viewport[1] = rect.top;
	//viewport[2] = rect.right - rect.left;
	//viewport[3] = rect.bottom - rect.top;

	// initialize extensions
	if(!initExtensions())
		dShaders = 0;

	reshape(rect.right, rect.bottom);
#endif
#ifdef RS_XSCREENSAVER
void initSaver(){
#endif
	// Seed random number generator
	srand((unsigned)time(NULL));

	// Limit memory consumption because the Windows previewer is just too darn slow
	if(doingPreview){
		dResolution = 6;
		if(dDepth > 3)
			dDepth = 3;
	};

	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);

	initFlares();

	thePath = new splinePath((dDepth * 2) + 6);

	if(dUseTunnels)
		theTunnel = new tunnel(thePath, 20);

	// To avoid popping, depth, which will be used for fogging, is set to
	// dDepth * goo grid size - size of one goo cubelet
	depth = float(dDepth) * 2.0f - 2.0f / float(dResolution);

	if(dUseGoo)
		theGoo = new goo(dResolution, depth);

	stars = new stretchedParticle*[dStars];
	for(int i=0; i<dStars; i++){
		stars[i] = new stretchedParticle;
		stars[i]->radius = rsRandf(float(dStarSize) * 0.0005f) + float(dStarSize) * 0.0005f;
		if(i % 10){  // usually bland stars
			stars[i]->color[0] = 0.8f + rsRandf(0.2f);
			stars[i]->color[1] = 0.8f + rsRandf(0.2f);
			stars[i]->color[2] = 0.8f + rsRandf(0.2f);
		}
		else{  // occasionally a colorful one
			stars[i]->color[0] = 0.3f + rsRandf(0.7f);
			stars[i]->color[1] = 0.3f + rsRandf(0.7f);
			stars[i]->color[2] = 0.3f + rsRandf(0.7f);
			stars[i]->color[rsRandi(3)] = 1.0f;
		}
		stars[i]->color[rsRandi(3)] = 1.0f;
		stars[i]->pos[0] = rsRandf(2.0f * depth) - depth;
		stars[i]->pos[1] = rsRandf(4.0f) - 2.0f;
		stars[i]->pos[2] = rsRandf(2.0f * depth) - depth;
		stars[i]->fov = float(dFov);
	}

	sunStar = new stretchedParticle;
	sunStar->radius = float(dStarSize) * 0.004f;
	sunStar->pos[0] = 0.0f;
	sunStar->pos[1] = 2.0f;
	sunStar->pos[2] = 0.0f;
	sunStar->fov = float(dFov);

	theStarBurst = new starBurst;
	for(int i=0; i<SB_NUM_STARS; i++)
		theStarBurst->stars[i]->radius = rsRandf(float(dStarSize) * 0.001f) + float(dStarSize) * 0.001f;

	glGenTextures(1, &nebulatex);
	if(dShaders){
		initShaders();
		numAnimTexFrames = 20;
		glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, nebulatex);
		glTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gluBuild2DMipmaps(GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB, 3, NEBULAMAPSIZE, NEBULAMAPSIZE, GL_RGB, GL_UNSIGNED_BYTE, nebulamap);
		gluBuild2DMipmaps(GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB, 3, NEBULAMAPSIZE, NEBULAMAPSIZE, GL_RGB, GL_UNSIGNED_BYTE, nebulamap);
		gluBuild2DMipmaps(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB, 3, NEBULAMAPSIZE, NEBULAMAPSIZE, GL_RGB, GL_UNSIGNED_BYTE, nebulamap);
		gluBuild2DMipmaps(GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB, 3, NEBULAMAPSIZE, NEBULAMAPSIZE, GL_RGB, GL_UNSIGNED_BYTE, nebulamap);
		gluBuild2DMipmaps(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB, 3, NEBULAMAPSIZE, NEBULAMAPSIZE, GL_RGB, GL_UNSIGNED_BYTE, nebulamap);
		gluBuild2DMipmaps(GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB, 3, NEBULAMAPSIZE, NEBULAMAPSIZE, GL_RGB, GL_UNSIGNED_BYTE, nebulamap);
	}
	else{
		//unsigned char spheremap[NEBULAMAPSIZE][NEBULAMAPSIZE][3] = nebulamap;
		numAnimTexFrames = 60;
		float x, y, temp;
		const int halfsize(NEBULAMAPSIZE / 2);
		for(int i=0; i<NEBULAMAPSIZE; ++i){
			for(int j=0; j<NEBULAMAPSIZE; ++j){
				x = float(i - halfsize) / float(halfsize);
				y = float(j - halfsize) / float(halfsize);
				temp = (x * x) + (y * y);
				if(temp > 1.0f)
					temp = 1.0f;
				if(temp < 0.0f)
					temp = 0.0f;
				temp = temp * temp;
				temp = temp * temp;
				nebulamap[i][j][0] = (unsigned char)(float(nebulamap[i][j][0]) * temp);
				nebulamap[i][j][1] = (unsigned char)(float(nebulamap[i][j][1]) * temp);
				nebulamap[i][j][2] = (unsigned char)(float(nebulamap[i][j][2]) * temp);
			}
		}
		glEnable(GL_NORMALIZE);
		glBindTexture(GL_TEXTURE_2D, nebulatex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		gluBuild2DMipmaps(GL_TEXTURE_2D, 3, NEBULAMAPSIZE, NEBULAMAPSIZE, GL_RGB, GL_UNSIGNED_BYTE, nebulamap);
	}

	glEnable(GL_FOG);
	float fog_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	glFogfv(GL_FOG_COLOR, fog_color);
	glFogf(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, depth * 0.7f);
	glFogf(GL_FOG_END, depth);

	// Initialize text
	textwriter = new rsText;

	//outfile.open("outfile");

	readyToDraw = 1;
}


#ifdef RS_XSCREENSAVER
void cleanUp(){
	// Free memory
	if(dUseGoo)
		delete theGoo;
	if(dUseTunnels){
		delete theTunnel;
		delete theCausticTextures;
		theCausticTextures = NULL;
	}
	delete thePath;
	delete theWNCM;
	theWNCM = NULL;
}
#endif


#ifdef WIN32
void cleanUp(HWND hwnd){
	// Free memory
	if(dUseGoo)
		delete theGoo;
	if(dUseTunnels){
		delete theTunnel;
		delete theCausticTextures;
		theCausticTextures = NULL;
	}
	delete thePath;
	delete theWNCM;
	theWNCM = NULL;

	// Kill device context
	ReleaseDC(hwnd, hdc);
	wglMakeCurrent(NULL, NULL);
	wglDeleteContext(hglrc);
}


// Initialize all user-defined stuff
void readRegistry(){
	LONG result;
	HKEY skey;
	DWORD valtype, valsize, val;

	setDefaults();

	result = RegOpenKeyEx(HKEY_CURRENT_USER, registryPath, 0, KEY_READ, &skey);
	if(result != ERROR_SUCCESS)
		return;

	valsize=sizeof(val);

	result = RegQueryValueEx(skey, "Speed", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dSpeed = val;
	result = RegQueryValueEx(skey, "Stars", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dStars = val;
	result = RegQueryValueEx(skey, "StarSize", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dStarSize = val;
	result = RegQueryValueEx(skey, "Resolution", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dResolution = val;
	result = RegQueryValueEx(skey, "Depth", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dDepth = val;
	result = RegQueryValueEx(skey, "Fov", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dFov = val;
	result = RegQueryValueEx(skey, "UseTunnels", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dUseTunnels = val;
	result = RegQueryValueEx(skey, "UseGoo", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dUseGoo = val;
	result = RegQueryValueEx(skey, "Shaders", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dShaders = val;
	result = RegQueryValueEx(skey, "FrameRateLimit", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dFrameRateLimit = val;

	RegCloseKey(skey);
}


// Save all user-defined stuff
void writeRegistry(){
    LONG result;
	HKEY skey;
	DWORD val, disp;

	result = RegCreateKeyEx(HKEY_CURRENT_USER, registryPath, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &skey, &disp);
	if(result != ERROR_SUCCESS)
		return;

	val = dSpeed;
	RegSetValueEx(skey, "Speed", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dStars;
	RegSetValueEx(skey, "Stars", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dStarSize;
	RegSetValueEx(skey, "StarSize", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dResolution;
	RegSetValueEx(skey, "Resolution", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dDepth;
	RegSetValueEx(skey, "Depth", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dFov;
	RegSetValueEx(skey, "Fov", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dUseTunnels;
	RegSetValueEx(skey, "UseTunnels", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dUseGoo;
	RegSetValueEx(skey, "UseGoo", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dShaders;
	RegSetValueEx(skey, "Shaders", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dFrameRateLimit;
	RegSetValueEx(skey, "FrameRateLimit", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));

	RegCloseKey(skey);
}


INT_PTR CALLBACK aboutProc(HWND hdlg, UINT msg, WPARAM wpm, LPARAM lpm){
	switch(msg){
	case WM_CTLCOLORSTATIC:
		if(HWND(lpm) == GetDlgItem(hdlg, WEBPAGE)){
			SetTextColor(HDC(wpm), RGB(0,0,255));
			SetBkColor(HDC(wpm), COLORREF(GetSysColor(COLOR_3DFACE)));
			return (INT_PTR)GetSysColorBrush(COLOR_3DFACE);
		}
		break;
    case WM_COMMAND:
		switch(LOWORD(wpm)){
		case IDOK:
		case IDCANCEL:
			EndDialog(hdlg, LOWORD(wpm));
			break;
		case WEBPAGE:
			ShellExecute(NULL, "open", "http://www.reallyslick.com/", NULL, NULL, SW_SHOWNORMAL);
		}
	}
	return FALSE;
}


void initControls(HWND hdlg){
	char cval[16];

	CheckDlgButton(hdlg, USETUNNELS, dUseTunnels);
	CheckDlgButton(hdlg, USEGOO, dUseGoo);
	CheckDlgButton(hdlg, USESHADERS, dShaders);

	SendDlgItemMessage(hdlg, SPEED, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(100))));
	SendDlgItemMessage(hdlg, SPEED, TBM_SETPOS, 1, LPARAM(dSpeed));
	SendDlgItemMessage(hdlg, SPEED, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, SPEED, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dSpeed);
	SendDlgItemMessage(hdlg, SPEEDTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, STARS, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(0), DWORD(10000))));
	SendDlgItemMessage(hdlg, STARS, TBM_SETPOS, 1, LPARAM(dStars));
	SendDlgItemMessage(hdlg, STARS, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, STARS, TBM_SETPAGESIZE, 0, LPARAM(50));
	sprintf_s(cval, "%d", dStars);
	SendDlgItemMessage(hdlg, STARSTEXT, WM_SETTEXT, 0, LPARAM(cval));
	
	SendDlgItemMessage(hdlg, STARSIZE, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(100))));
	SendDlgItemMessage(hdlg, STARSIZE, TBM_SETPOS, 1, LPARAM(dStarSize));
	SendDlgItemMessage(hdlg, STARSIZE, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, STARSIZE, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dStarSize);
	SendDlgItemMessage(hdlg, STARSIZETEXT, WM_SETTEXT, 0, LPARAM(cval));
	
	SendDlgItemMessage(hdlg, RESOLUTION, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(4), DWORD(20))));
	SendDlgItemMessage(hdlg, RESOLUTION, TBM_SETPOS, 1, LPARAM(dResolution));
	SendDlgItemMessage(hdlg, RESOLUTION, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, RESOLUTION, TBM_SETPAGESIZE, 0, LPARAM(2));
	if(dUseGoo)
		EnableWindow(GetDlgItem(hdlg, RESOLUTION), TRUE);
	else
		EnableWindow(GetDlgItem(hdlg, RESOLUTION), FALSE);
	sprintf_s(cval, "%d", dResolution);
	SendDlgItemMessage(hdlg, RESOLUTIONTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, DEPTH, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(10))));
	SendDlgItemMessage(hdlg, DEPTH, TBM_SETPOS, 1, LPARAM(dDepth));
	SendDlgItemMessage(hdlg, DEPTH, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, DEPTH, TBM_SETPAGESIZE, 0, LPARAM(2));
	sprintf_s(cval, "%d", dDepth);
	SendDlgItemMessage(hdlg, DEPTHTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, FOV, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(10), DWORD(150))));
	SendDlgItemMessage(hdlg, FOV, TBM_SETPOS, 1, LPARAM(dFov));
	SendDlgItemMessage(hdlg, FOV, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, FOV, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dFov);
	SendDlgItemMessage(hdlg, FOVTEXT, WM_SETTEXT, 0, LPARAM(cval));

	initFrameRateLimitSlider(hdlg, FRAMERATELIMIT, FRAMERATELIMITTEXT);
}


INT_PTR CALLBACK screenSaverConfigureDialog(HWND hdlg, UINT msg, WPARAM wpm, LPARAM lpm){
	int ival;
	char cval[16];

    switch(msg){
    case WM_INITDIALOG:
        InitCommonControls();
        readRegistry();
        initControls(hdlg);
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wpm)){
        case IDOK:
			dSpeed = SendDlgItemMessage(hdlg, SPEED, TBM_GETPOS, 0, 0);
			dStars = SendDlgItemMessage(hdlg, STARS, TBM_GETPOS, 0, 0);
			dStarSize = SendDlgItemMessage(hdlg, STARSIZE, TBM_GETPOS, 0, 0);
			dResolution = SendDlgItemMessage(hdlg, RESOLUTION, TBM_GETPOS, 0, 0);
			dDepth = SendDlgItemMessage(hdlg, DEPTH, TBM_GETPOS, 0, 0);
			dFov = SendDlgItemMessage(hdlg, FOV, TBM_GETPOS, 0, 0);
			dUseTunnels = (IsDlgButtonChecked(hdlg, USETUNNELS) == BST_CHECKED);
			dUseGoo = (IsDlgButtonChecked(hdlg, USEGOO) == BST_CHECKED);
			dShaders = (IsDlgButtonChecked(hdlg, USESHADERS) == BST_CHECKED);
			dFrameRateLimit = SendDlgItemMessage(hdlg, FRAMERATELIMIT, TBM_GETPOS, 0, 0);
			writeRegistry();
            // Fall through
        case IDCANCEL:
            EndDialog(hdlg, LOWORD(wpm));
            break;
		case DEFAULTS:
			setDefaults();
			initControls(hdlg);
			break;
		case ABOUT:
			DialogBox(mainInstance, MAKEINTRESOURCE(DLG_ABOUT), hdlg, aboutProc);
			break;
		case USEGOO:
			if(IsDlgButtonChecked(hdlg, USEGOO) == BST_CHECKED)
				EnableWindow(GetDlgItem(hdlg, RESOLUTION), TRUE);
			else
				EnableWindow(GetDlgItem(hdlg, RESOLUTION), FALSE);
			break;
		}
        return TRUE;
	case WM_HSCROLL:
		if(HWND(lpm) == GetDlgItem(hdlg, SPEED)){
			ival = SendDlgItemMessage(hdlg, SPEED, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, SPEEDTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, STARS)){
			ival = SendDlgItemMessage(hdlg, STARS, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, STARSTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, STARSIZE)){
			ival = SendDlgItemMessage(hdlg, STARSIZE, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, STARSIZETEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, RESOLUTION)){
			ival = SendDlgItemMessage(hdlg, RESOLUTION, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, RESOLUTIONTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, DEPTH)){
			ival = SendDlgItemMessage(hdlg, DEPTH, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, DEPTHTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, FOV)){
			ival = SendDlgItemMessage(hdlg, FOV, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, FOVTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, FRAMERATELIMIT))
			updateFrameRateLimitSlider(hdlg, FRAMERATELIMIT, FRAMERATELIMITTEXT);
		return TRUE;
    }
    return FALSE;
}


LONG screenSaverProc(HWND hwnd, UINT msg, WPARAM wpm, LPARAM lpm){
	switch(msg){
	case WM_CREATE:
		readRegistry();
		initSaver(hwnd);
		break;
	case WM_DESTROY:
		readyToDraw = 0;
		cleanUp(hwnd);
		break;
	}
	return defScreenSaverProc(hwnd, msg, wpm, lpm);
}
#endif
