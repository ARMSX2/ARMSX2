/*
 * Copyright (C) 1999-2010  Terence M. Welsh
 *
 * This file is part of Flux.
 *
 * Flux is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Flux is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


// Flux screen saver

#ifdef WIN32
#include <windows.h>
#include <rsWin32Saver/rsWin32Saver.h>
#include <regstr.h>
#include <commctrl.h>
#include <time.h>
#include "resource.h"
#endif
#ifdef RS_XSCREENSAVER
#include <rsXScreenSaver/rsXScreenSaver.h>
#endif

#include <stdio.h>
#include <rsText/rsText.h>
#include <rsMath/rsMath.h>
#include <math.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <Rgbhsl/Rgbhsl.h>

#define NUMCONSTS 8
#define PIx2 6.28318530718f
#define DEG2RAD 0.0174532925f
#define LIGHTSIZE 64


class flux;
class particle;


// Global variables
#ifdef WIN32
LPCTSTR registryPath = ("Software\\Really Slick\\Flux");
HGLRC hglrc;
HDC hdc;
#endif
int readyToDraw = 0;
unsigned int tex;
flux *fluxes;
float lumdiff;
int whichparticle;
float cosCameraAngle, sinCameraAngle;
unsigned char lightTexture[LIGHTSIZE][LIGHTSIZE];
float aspectRatio;
float frameTime = 0.0f;
float orbitiness = 0.0f;
float prevOrbitiness = 0.0f;
// text output
rsText* textwriter;
// Parameters edited in the dialog box
int dFluxes;
int dParticles;
int dTrail;
int dGeometry;
int dSize;
int dComplexity;
int dRandomize;
int dExpansion;
int dRotation;
int dWind;
int dInstability;
int dBlur;
#ifdef RS_XSCREENSAVER
#define DEFAULTS1 1
#define DEFAULTS2 2
#define DEFAULTS3 3
#define DEFAULTS4 4
#define DEFAULTS5 5
#define DEFAULTS6 6
#endif


// This class is poorly named.  It's actually a whole trail of particles.
class particle{
public:
	float **vertices;
	int counter;
	float offset[3];

	particle();
	~particle();
	void update(float *c);
};

particle::particle(){
	int i;

	// Offsets are somewhat like default positions for the head of each
	// particle trail.  Offsets spread out the particle trails and keep
	// them from all overlapping.
	offset[0] = cosf(PIx2 * float(whichparticle) / float(dParticles));
	offset[1] = float(whichparticle) / float(dParticles) - 0.5f;
	offset[2] = sinf(PIx2 * float(whichparticle) / float(dParticles));
	whichparticle++;

	// Initialize memory and set initial positions out of view of the camera
	vertices = new float*[dTrail];
	for(i=0; i<dTrail; i++){
		vertices[i] = new float[5];  // 0,1,2 = position, 3 = hue, 4 = saturation
		vertices[i][0] = 0.0f;
		vertices[i][1] = 3.0f;
		vertices[i][2] = 0.0f;
		vertices[i][3] = 0.0f;
		vertices[i][4] = 0.0f;
	}

	counter = 0;
}

particle::~particle(){
	for(int i=0; i<dTrail; i++)
		delete[] vertices[i];
	delete[] vertices;
}

void particle::update(float *c){
	int i, p, growth;
	float rgb[3];
	float cx, cy, cz;  // Containment variables
	float luminosity;
	static float expander = 1.0f + 0.0005f * float(dExpansion);
	static float blower = 0.001f * float(dWind);
	static float otherxyz[3];
	float depth;

	// Record old position
	int oldc = counter;

	counter ++;
	if(counter >= dTrail)
		counter = 0;

	// Here's the iterative math for calculating new vertex positions
	// first calculate limiting terms which keep vertices from constantly
	// flying off to infinity
	cx = vertices[oldc][0] * (1.0f - 1.0f / (vertices[oldc][0] * vertices[oldc][0] + 1.0f));
	cy = vertices[oldc][1] * (1.0f - 1.0f / (vertices[oldc][1] * vertices[oldc][1] + 1.0f));
	cz = vertices[oldc][2] * (1.0f - 1.0f / (vertices[oldc][2] * vertices[oldc][2] + 1.0f));
	// then calculate new positions
	vertices[counter][0] = vertices[oldc][0] + c[6] * offset[0] - cx
		+ c[2] * vertices[oldc][1]
		+ c[5] * vertices[oldc][2];
	vertices[counter][1] = vertices[oldc][1] + c[6] * offset[1] - cy
		+ c[1] * vertices[oldc][2]
		+ c[4] * vertices[oldc][0];
	vertices[counter][2] = vertices[oldc][2] + c[6] * offset[2] - cz
		+ c[0] * vertices[oldc][0]
		+ c[3] * vertices[oldc][1];

	// calculate "orbitiness" of particles
	const float xdiff(vertices[counter][0] - vertices[oldc][0]);
	const float ydiff(vertices[counter][1] - vertices[oldc][1]);
	const float zdiff(vertices[counter][2] - vertices[oldc][2]);
	const float distsq(vertices[counter][0] * vertices[counter][0]
		+ vertices[counter][1] * vertices[counter][1]
		+ vertices[counter][2] * vertices[counter][2]);
	const float oldDistsq(vertices[oldc][0] * vertices[oldc][0]
		+ vertices[oldc][1] * vertices[oldc][1]
		+ vertices[oldc][2] * vertices[oldc][2]);
	orbitiness += (xdiff * xdiff + ydiff * ydiff + zdiff * zdiff)
		/ (2.0f - fabs(distsq - oldDistsq));

	// Pick a hue
	vertices[counter][3] = cx * cx + cy * cy + cz * cz;
	if(vertices[counter][3] > 1.0f)
		vertices[counter][3] = 1.0f;
	vertices[counter][3] += c[7];
	// Limit the hue (0 - 1)
	if(vertices[counter][3] > 1.0f)
		vertices[counter][3] -= 1.0f;
	if(vertices[counter][3] < 0.0f)
		vertices[counter][3] += 1.0f;
	// Pick a saturation
	vertices[counter][4] = c[0] + vertices[counter][3];
	// Limit the saturation (0 - 1)
	if(vertices[counter][4] < 0.0f)
		vertices[counter][4] = -vertices[counter][4];
	vertices[counter][4] -= float(int(vertices[counter][4]));
	vertices[counter][4] = 1.0f - (vertices[counter][4] * vertices[counter][4]);

	// Bring particles back if they escape
	if(!counter){
		if(vertices[counter][0] * vertices[counter][0]
			+ vertices[counter][1] * vertices[counter][1]
			+ vertices[counter][2] * vertices[counter][2] > 100000000.0f){
			vertices[counter][0] = rsRandf(2.0f) - 1.0f;
			vertices[counter][1] = rsRandf(2.0f) - 1.0f;
			vertices[counter][2] = rsRandf(2.0f) - 1.0f;
		}
	}

	// Draw every vertex in particle trail
	p = counter;
	growth = 0;
	luminosity = lumdiff;
	for(i=0; i<dTrail; i++){
		p ++;
		if(p >= dTrail)
			p = 0;
		growth++;

		if(vertices[p][0] * vertices[p][0]
			+ vertices[p][1] * vertices[p][1]
			+ vertices[p][2] * vertices[p][2] < 40000.0f){

			// assign color to particle
			hsl2rgb(vertices[p][3], vertices[p][4], luminosity, rgb[0], rgb[1], rgb[2]);
			glColor3fv(rgb);

			glPushMatrix();
			switch(dGeometry){
			case 0:  // Points
				depth = cosCameraAngle * vertices[p][2] - sinCameraAngle * vertices[p][0];
				glTranslatef(cosCameraAngle * vertices[p][0] + sinCameraAngle
					* vertices[p][2], vertices[p][1], depth);
				switch(dTrail - growth){
				case 0:
					glPointSize(float(dSize * (depth + 200.0f) * 0.001036f));
					break;
				case 1:
					glPointSize(float(dSize * (depth + 200.0f) * 0.002f));
					break;
				case 2:
					glPointSize(float(dSize * (depth + 200.0f) * 0.002828f));
					break;
				case 3:
					glPointSize(float(dSize * (depth + 200.0f) * 0.003464f));
					break;
				case 4:
					glPointSize(float(dSize * (depth + 200.0f) * 0.003864f));
					break;
				default:
					glPointSize(float(dSize * (depth + 200.0f) * 0.004f));
				}
				glBegin(GL_POINTS);
					glVertex3f(0.0f,0.0f,0.0f);
				glEnd();
				break;
			case 1:  // Spheres
				glTranslatef(vertices[p][0], vertices[p][1], vertices[p][2]);
				switch(dTrail - growth){
				case 0:
					glScalef(0.259f, 0.259f, 0.259f);
					break;
				case 1:
					glScalef(0.5f, 0.5f, 0.5f);
					break;
				case 2:
					glScalef(0.707f, 0.707f, 0.707f);
					break;
				case 3:
					glScalef(0.866f, 0.866f, 0.866f);
					break;
				case 4:
					glScalef(0.966f, 0.966f, 0.966f);
				}
				glCallList(1);
				break;
			case 2:  // Lights
				depth = cosCameraAngle * vertices[p][2] - sinCameraAngle * vertices[p][0];
				glTranslatef(cosCameraAngle * vertices[p][0] + sinCameraAngle
					* vertices[p][2], vertices[p][1], depth);
				switch(dTrail - growth){
				case 0:
					glScalef(0.259f, 0.259f, 0.259f);
					break;
				case 1:
					glScalef(0.5f, 0.5f, 0.5f);
					break;
				case 2:
					glScalef(0.707f, 0.707f, 0.707f);
					break;
				case 3:
					glScalef(0.866f, 0.866f, 0.866f);
					break;
				case 4:
					glScalef(0.966f, 0.966f, 0.966f);
				}
				glCallList(1);
			}
			glPopMatrix();
		}

		vertices[p][0] *= expander;
		vertices[p][1] *= expander;
		vertices[p][2] *= expander;
		vertices[p][2] += blower;
		luminosity += lumdiff;
	}
}


// This class is a set of particle trails and constants that enter
// into their equations of motion.
class flux{
public:
	particle *particles;
	int randomize;
	float c[NUMCONSTS];     // constants
	float cv[NUMCONSTS];    // constants' change velocities

	flux();
	~flux();
	void update();
};

flux::flux(){
	int i;

	whichparticle = 0;

	particles = new particle[dParticles];
	randomize = 1;
	for(i=0; i<NUMCONSTS; i++){
		c[i] = rsRandf(2.0f) - 1.0f;
		cv[i] = rsRandf(0.000005f * float(dInstability) * float(dInstability))
			+ 0.000001f * float(dInstability) * float(dInstability);
	}
}

flux::~flux(){
	delete[] particles;
}

void flux::update(){
	int i;

	// randomize constants
	if(dRandomize){
		randomize --;
		if(randomize <= 0){
			for(i=0; i<NUMCONSTS; i++)
				c[i] = rsRandf(2.0f) - 1.0f;
			int temp = 101 - dRandomize;
			temp = temp * temp;
			randomize = temp + rsRandi(temp);
		}
	}

	// update constants
	for(i=0; i<NUMCONSTS; i++){
		c[i] += cv[i];
		if(c[i] >= 1.0f){
			c[i] = 1.0f;
			cv[i] = -cv[i];
		}
		if(c[i] <= -1.0f){
			c[i] = -1.0f;
			cv[i] = -cv[i];
		}
	}

	prevOrbitiness = orbitiness;
	orbitiness = 0.0f;

	// update all particles in this flux field
	for(i=0; i<dParticles; i++)
		particles[i].update(c);

/*	if(orbitiness < prevOrbitiness){
		i = rsRandi(NUMCONSTS - 1);
		cv[i] = -cv[i];
	}*/
}


void draw(){
	int i;

	// clear the screen
	if(dBlur){  // partially
		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
			glLoadIdentity();
			glOrtho(0.0, 1.0, 0.0, 1.0, 1.0, -1.0);
			glMatrixMode(GL_MODELVIEW);
			glPushMatrix();
				glLoadIdentity();
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glEnable(GL_BLEND);
				glDisable(GL_DEPTH_TEST);
				glColor4f(0.0f, 0.0f, 0.0f, 0.5f - (float(sqrtf(sqrtf(float(dBlur)))) * 0.15495f));
				glBegin(GL_TRIANGLE_STRIP);
					glVertex3f(0.0f, 0.0f, 0.0f);
					glVertex3f(1.0f, 0.0f, 0.0f);
					glVertex3f(0.0f, 1.0f, 0.0f);
					glVertex3f(1.0f, 1.0f, 0.0f);
				glEnd();
			glPopMatrix();
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
	}
	else  // completely
		glClear(GL_COLOR_BUFFER_BIT);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0, 0.0, -2.5);

	// Rotate camera
	static float cameraAngle = 0.0f;
	cameraAngle += 0.01f * float(dRotation);
	if(cameraAngle >= 360.0f)
		cameraAngle -= 360.0f;
	if(dGeometry == 1)  // Only rotate for spheres
		glRotatef(cameraAngle, 0.0f, 1.0f, 0.0f);
	else{
		cosCameraAngle = cosf(cameraAngle * DEG2RAD);
		sinCameraAngle = sinf(cameraAngle * DEG2RAD);
	}

	// set up state for rendering particles
	switch(dGeometry){
	case 0:  // Blending for points
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glEnable(GL_BLEND);
		glEnable(GL_POINT_SMOOTH);
		glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
		break;
	case 1:  // No blending for spheres, but we need z-buffering
		glDisable(GL_BLEND);
		glEnable(GL_DEPTH_TEST);
		glClear(GL_DEPTH_BUFFER_BIT);
		break;
	case 2:  // Blending for lights
		glBlendFunc(GL_ONE, GL_ONE);
		glEnable(GL_BLEND);
		glBindTexture(GL_TEXTURE_2D, tex);
		glEnable(GL_TEXTURE_2D);
	}

	// Update particles
	glMatrixMode(GL_MODELVIEW);
	for(i=0; i<dFluxes; i++)
		fluxes[i].update();

	// print text
	static float totalTime = 0.0f;
	totalTime += frameTime;
	static std::string str;
	static int frames = 0;
	++frames;
	if(frames == 20){
		str = "FPS = " + to_string(20.0f / totalTime);
		totalTime = 0.0f;
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
		textwriter->draw(str);

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


void setDefaults(int which){
	switch(which){
	case DEFAULTS1:  // Regular
		dFluxes = 1;
		dParticles = 20;
		dTrail = 40;
		dGeometry = 2;
		dSize = 15;
		dComplexity = 3;
		dRandomize = 0;
		dExpansion = 40;
		dRotation = 30;
		dWind = 20;
		dInstability = 20;
		dBlur = 0;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS2:  // Hypnotic
		dFluxes = 2;
		dParticles = 10;
		dTrail = 40;
		dGeometry = 2;
		dSize = 15;
		dRandomize = 80;
		dExpansion = 20;
		dRotation = 0;
		dWind = 40;
		dInstability = 10;
		dBlur = 30;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS3:  // Insane
		dFluxes = 4;
		dParticles = 30;
		dTrail = 8;
		dGeometry = 2;
		dSize = 25;
		dRandomize = 0;
		dExpansion = 80;
		dRotation = 60;
		dWind = 40;
		dInstability = 100;
		dBlur = 10;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS4:  // Sparklers
		dFluxes = 3;
		dParticles = 20;
		dTrail = 6;
		dGeometry = 1;
		dSize = 20;
		dComplexity = 3;
		dRandomize = 85;
		dExpansion = 60;
		dRotation = 30;
		dWind = 20;
		dInstability = 30;
		dBlur = 0;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS5:  // Paradigm
		dFluxes = 1;
		dParticles = 40;
		dTrail = 40;
		dGeometry = 2;
		dSize = 5;
		dRandomize = 90;
		dExpansion = 30;
		dRotation = 20;
		dWind = 10;
		dInstability = 5;
		dBlur = 10;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS6:  // Galactic
		dFluxes = 3;
		dParticles = 2;
		dTrail = 1500;
		dGeometry = 2;
		dSize = 10;
		dRandomize = 0;
		dExpansion = 5;
		dRotation = 25;
		dWind = 0;
		dInstability = 5;
		dBlur = 0;
		dFrameRateLimit = 60;
	}
}


#ifdef RS_XSCREENSAVER
void handleCommandLine(int argc, char* argv[]){
	int defaults = DEFAULTS1;
	setDefaults(defaults);
	getArgumentsValue(argc, argv, std::string("-default"), defaults, DEFAULTS1, DEFAULTS6);
	setDefaults(defaults);
	getArgumentsValue(argc, argv, std::string("-fluxes"), dFluxes, 1, 100);
	getArgumentsValue(argc, argv, std::string("-particles"), dParticles, 1, 1000);
	getArgumentsValue(argc, argv, std::string("-trail"), dTrail, 3, 10000);
	getArgumentsValue(argc, argv, std::string("-geometry"), dGeometry, 0, 2);
	getArgumentsValue(argc, argv, std::string("-size"), dSize, 1, 100);
	getArgumentsValue(argc, argv, std::string("-complexity"), dComplexity, 1, 10);
	getArgumentsValue(argc, argv, std::string("-randomize"), dRandomize, 0, 100);
	getArgumentsValue(argc, argv, std::string("-expansion"), dExpansion, 0, 100);
	getArgumentsValue(argc, argv, std::string("-rotation"), dRotation, 1, 100);
	getArgumentsValue(argc, argv, std::string("-wind"), dWind, 1, 10);
	getArgumentsValue(argc, argv, std::string("-instability"), dInstability, 1, 100);
	getArgumentsValue(argc, argv, std::string("-blur"), dBlur, 0, 100);
}

void reshape(int width, int height){
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	aspectRatio = float(width) / float(height);
	gluPerspective(100.0, aspectRatio, 0.01, 200.0);
	glMatrixMode(GL_MODELVIEW);
}
#endif


#ifdef WIN32
void initSaver(HWND hwnd){
	RECT rect;
	
	// Window initialization
	hdc = GetDC(hwnd);
	setBestPixelFormat(hdc);
	hglrc = wglCreateContext(hdc);
	GetClientRect(hwnd, &rect);
	wglMakeCurrent(hdc, hglrc);
	glViewport(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
	aspectRatio = float(rect.right) / float(rect.bottom);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(100.0, aspectRatio, 0.01, 200.0);
#endif
#ifdef RS_XSCREENSAVER
void initSaver(){
#endif
	int i, j;
	float x, y, temp;

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	if(dGeometry == 0){
		glEnable(GL_POINT_SMOOTH);
		//glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
	}

	glFrontFace(GL_CCW);
	glEnable(GL_CULL_FACE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	if(dGeometry == 1){  // Spheres and their lighting
		glNewList(1, GL_COMPILE);
			GLUquadricObj *qobj = gluNewQuadric();
			gluSphere(qobj, 0.005f * float(dSize), dComplexity + 2, dComplexity + 1);
			gluDeleteQuadric(qobj);
		glEndList();

		glEnable(GL_LIGHTING);
		glEnable(GL_LIGHT0);
		float ambient[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		float diffuse[4] = {1.0f, 1.0f, 1.0f, 0.0f};
		float specular[4] = {1.0f, 1.0f, 1.0f, 0.0f};
		float position[4] = {500.0f, 500.0f, 500.0f, 0.0f};
		glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);
		glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
		glLightfv(GL_LIGHT0, GL_SPECULAR, specular);
		glLightfv(GL_LIGHT0, GL_POSITION, position);
		glEnable(GL_COLOR_MATERIAL);
		glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
	}

	if(dGeometry == 2){  // Init lights
		for(i=0; i<LIGHTSIZE; i++){
			for(j=0; j<LIGHTSIZE; j++){
				x = float(i - LIGHTSIZE / 2) / float(LIGHTSIZE / 2);
				y = float(j - LIGHTSIZE / 2) / float(LIGHTSIZE / 2);
				temp = 1.0f - float(sqrt((x * x) + (y * y)));
				if(temp > 1.0f)
					temp = 1.0f;
				if(temp < 0.0f)
					temp = 0.0f;
				lightTexture[i][j] = static_cast<unsigned char>(255.0f * temp * temp);
			}
		}
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, 1, LIGHTSIZE, LIGHTSIZE, 0,
			GL_LUMINANCE, GL_UNSIGNED_BYTE, lightTexture);

		temp = float(dSize) * 0.005f;
		glNewList(1, GL_COMPILE);
			glBegin(GL_TRIANGLES);
				glTexCoord2f(0.0f, 0.0f);
				glVertex3f(-temp, -temp, 0.0f);
				glTexCoord2f(1.0f, 0.0f);
				glVertex3f(temp, -temp, 0.0f);
				glTexCoord2f(1.0f, 1.0f);
				glVertex3f(temp, temp, 0.0f);
				glTexCoord2f(0.0f, 0.0f);
				glVertex3f(-temp, -temp, 0.0f);
				glTexCoord2f(1.0f, 1.0f);
				glVertex3f(temp, temp, 0.0f);
				glTexCoord2f(0.0f, 1.0f);
				glVertex3f(-temp, temp, 0.0f);
			glEnd();
		glEndList();
	}

	// Initialize luminosity difference
	lumdiff = 1.0f / float(dTrail);

	// Initialize flux fields
	fluxes = new flux[dFluxes];

	// Initialize text
	textwriter = new rsText;
	
	readyToDraw = 1;
}


#ifdef RS_XSCREENSAVER
void cleanUp(){
	// Free memory
	delete[] fluxes;
}
#endif


#ifdef WIN32
void cleanUp(HWND hwnd){
	// Free memory
	delete[] fluxes;

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

	setDefaults(DEFAULTS1);

	result = RegOpenKeyEx(HKEY_CURRENT_USER, registryPath, 0, KEY_READ, &skey);
	if(result != ERROR_SUCCESS)
		return;

	valsize=sizeof(val);

	result = RegQueryValueEx(skey, "Fluxes", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dFluxes = val;
	result = RegQueryValueEx(skey, "Particles", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dParticles = val;
	result = RegQueryValueEx(skey, "Trail", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dTrail = val;
	result = RegQueryValueEx(skey, "Geometry", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dGeometry = val;
	result = RegQueryValueEx(skey, "Size", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dSize = val;
	result = RegQueryValueEx(skey, "Complexity", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dComplexity = val;
	result = RegQueryValueEx(skey, "Randomize", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dRandomize = val;
	result = RegQueryValueEx(skey, "Expansion", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dExpansion = val;
	result = RegQueryValueEx(skey, "Rotation", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dRotation = val;
	result = RegQueryValueEx(skey, "Wind", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dWind = val;
	result = RegQueryValueEx(skey, "Instability", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dInstability = val;
	result = RegQueryValueEx(skey, "Blur", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dBlur = val;
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

	val = dFluxes;
	RegSetValueEx(skey, "Fluxes", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dParticles;
	RegSetValueEx(skey, "Particles", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dTrail;
	RegSetValueEx(skey, "Trail", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dGeometry;
	RegSetValueEx(skey, "Geometry", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dSize;
	RegSetValueEx(skey, "Size", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dComplexity;
	RegSetValueEx(skey, "Complexity", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dRandomize;
	RegSetValueEx(skey, "Randomize", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dExpansion;
	RegSetValueEx(skey, "Expansion", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dRotation;
	RegSetValueEx(skey, "Rotation", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dWind;
	RegSetValueEx(skey, "Wind", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dInstability;
	RegSetValueEx(skey, "Instability", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dBlur;
	RegSetValueEx(skey, "Blur", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
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
	return false;
}


void initControls(HWND hdlg){
	char cval[16];

	SendDlgItemMessage(hdlg, FLUXES, UDM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(100), DWORD(1))));
	SendDlgItemMessage(hdlg, FLUXES, UDM_SETPOS, 0, LPARAM(dFluxes));

	SendDlgItemMessage(hdlg, PARTICLES, UDM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1000), DWORD(1))));
	SendDlgItemMessage(hdlg, PARTICLES, UDM_SETPOS, 0, LPARAM(dParticles));

	SendDlgItemMessage(hdlg, TRAIL, UDM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(10000), DWORD(3))));
	SendDlgItemMessage(hdlg, TRAIL, UDM_SETPOS, 0, LPARAM(dTrail));

	SendDlgItemMessage(hdlg, GEOMETRY, CB_DELETESTRING, WPARAM(2), 0);
	SendDlgItemMessage(hdlg, GEOMETRY, CB_DELETESTRING, WPARAM(1), 0);
	SendDlgItemMessage(hdlg, GEOMETRY, CB_DELETESTRING, WPARAM(0), 0);
	SendDlgItemMessage(hdlg, GEOMETRY, CB_ADDSTRING, 0, LPARAM("Points"));
	SendDlgItemMessage(hdlg, GEOMETRY, CB_ADDSTRING, 0, LPARAM("Spheres"));
	SendDlgItemMessage(hdlg, GEOMETRY, CB_ADDSTRING, 0, LPARAM("Lights"));
	SendDlgItemMessage(hdlg, GEOMETRY, CB_SETCURSEL, WPARAM(dGeometry), 0);

	SendDlgItemMessage(hdlg, SIZE, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(100))));
	SendDlgItemMessage(hdlg, SIZE, TBM_SETPOS, 1, LPARAM(dSize));
	SendDlgItemMessage(hdlg, SIZE, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, SIZE, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dSize);
	SendDlgItemMessage(hdlg, SIZETEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, COMPLEXITY, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(10))));
	SendDlgItemMessage(hdlg, COMPLEXITY, TBM_SETPOS, 1, LPARAM(dComplexity));
	SendDlgItemMessage(hdlg, COMPLEXITY, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, COMPLEXITY, TBM_SETPAGESIZE, 0, LPARAM(2));
	if(dGeometry == 1)
		EnableWindow(GetDlgItem(hdlg, COMPLEXITY), TRUE);
	else
		EnableWindow(GetDlgItem(hdlg, COMPLEXITY), FALSE);
	sprintf_s(cval, "%d", dComplexity);
	SendDlgItemMessage(hdlg, COMPLEXITYTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, RANDOMIZE, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(0), DWORD(100))));
	SendDlgItemMessage(hdlg, RANDOMIZE, TBM_SETPOS, 1, LPARAM(dRandomize));
	SendDlgItemMessage(hdlg, RANDOMIZE, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, RANDOMIZE, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dRandomize);
	SendDlgItemMessage(hdlg, RANDOMIZETEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, EXPANSION, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(0), DWORD(100))));
	SendDlgItemMessage(hdlg, EXPANSION, TBM_SETPOS, 1, LPARAM(dExpansion));
	SendDlgItemMessage(hdlg, EXPANSION, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, EXPANSION, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dExpansion);
	SendDlgItemMessage(hdlg, EXPANSIONTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, ROTATION, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(0), DWORD(100))));
	SendDlgItemMessage(hdlg, ROTATION, TBM_SETPOS, 1, LPARAM(dRotation));
	SendDlgItemMessage(hdlg, ROTATION, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, ROTATION, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dRotation);
	SendDlgItemMessage(hdlg, ROTATIONTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, WIND, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(0), DWORD(100))));
	SendDlgItemMessage(hdlg, WIND, TBM_SETPOS, 1, LPARAM(dWind));
	SendDlgItemMessage(hdlg, WIND, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, WIND, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dWind);
	SendDlgItemMessage(hdlg, WINDTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, INSTABILITY, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(100))));
	SendDlgItemMessage(hdlg, INSTABILITY, TBM_SETPOS, 1, LPARAM(dInstability));
	SendDlgItemMessage(hdlg, INSTABILITY, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, INSTABILITY, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dInstability);
	SendDlgItemMessage(hdlg, INSTABILITYTEXT, WM_SETTEXT, 0, LPARAM(cval));
	
	SendDlgItemMessage(hdlg, BLUR, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(0), DWORD(100))));
	SendDlgItemMessage(hdlg, BLUR, TBM_SETPOS, 1, LPARAM(dBlur));
	SendDlgItemMessage(hdlg, BLUR, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, BLUR, TBM_SETPAGESIZE, 0, LPARAM(5));
	sprintf_s(cval, "%d", dBlur);
	SendDlgItemMessage(hdlg, BLURTEXT, WM_SETTEXT, 0, LPARAM(cval));

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
			dFluxes = SendDlgItemMessage(hdlg, FLUXES, UDM_GETPOS, 0, 0);
            dParticles = SendDlgItemMessage(hdlg, PARTICLES, UDM_GETPOS, 0, 0);
			dTrail = SendDlgItemMessage(hdlg, TRAIL, UDM_GETPOS, 0, 0);
			dGeometry = SendDlgItemMessage(hdlg, GEOMETRY, CB_GETCURSEL, 0, 0);
			dSize = SendDlgItemMessage(hdlg, SIZE, TBM_GETPOS, 0, 0);
			dComplexity = SendDlgItemMessage(hdlg, COMPLEXITY, TBM_GETPOS, 0, 0);
			dRandomize = SendDlgItemMessage(hdlg, RANDOMIZE, TBM_GETPOS, 0, 0);
			dExpansion = SendDlgItemMessage(hdlg, EXPANSION, TBM_GETPOS, 0, 0);
			dRotation = SendDlgItemMessage(hdlg, ROTATION, TBM_GETPOS, 0, 0);
			dWind = SendDlgItemMessage(hdlg, WIND, TBM_GETPOS, 0, 0);
			dInstability = SendDlgItemMessage(hdlg, INSTABILITY, TBM_GETPOS, 0, 0);
			dBlur = SendDlgItemMessage(hdlg, BLUR, TBM_GETPOS, 0, 0);
			dFrameRateLimit = SendDlgItemMessage(hdlg, FRAMERATELIMIT, TBM_GETPOS, 0, 0);
			writeRegistry();
            // Fall through
        case IDCANCEL:
            EndDialog(hdlg, LOWORD(wpm));
            break;
		case DEFAULTS1:
			setDefaults(DEFAULTS1);
			initControls(hdlg);
			break;
		case DEFAULTS2:
			setDefaults(DEFAULTS2);
			initControls(hdlg);
			break;
		case DEFAULTS3:
			setDefaults(DEFAULTS3);
			initControls(hdlg);
			break;
		case DEFAULTS4:
			setDefaults(DEFAULTS4);
			initControls(hdlg);
			break;
		case DEFAULTS5:
			setDefaults(DEFAULTS5);
			initControls(hdlg);
			break;
		case DEFAULTS6:
			setDefaults(DEFAULTS6);
			initControls(hdlg);
			break;
		case ABOUT:
			DialogBox(mainInstance, MAKEINTRESOURCE(DLG_ABOUT), hdlg, aboutProc);
			break;
		case GEOMETRY:
			if(SendDlgItemMessage(hdlg, GEOMETRY, CB_GETCURSEL, 0, 0) == 1)
				EnableWindow(GetDlgItem(hdlg, COMPLEXITY), TRUE);
			else
				EnableWindow(GetDlgItem(hdlg, COMPLEXITY), FALSE);
		}
        return TRUE;
	case WM_HSCROLL:
		if(HWND(lpm) == GetDlgItem(hdlg, SIZE)){
			ival = SendDlgItemMessage(hdlg, SIZE, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, SIZETEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, COMPLEXITY)){
			ival = SendDlgItemMessage(hdlg, COMPLEXITY, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, COMPLEXITYTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, RANDOMIZE)){
			ival = SendDlgItemMessage(hdlg, RANDOMIZE, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, RANDOMIZETEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, EXPANSION)){
			ival = SendDlgItemMessage(hdlg, EXPANSION, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, EXPANSIONTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, ROTATION)){
			ival = SendDlgItemMessage(hdlg, ROTATION, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, ROTATIONTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, WIND)){
			ival = SendDlgItemMessage(hdlg, WIND, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, WINDTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, INSTABILITY)){
			ival = SendDlgItemMessage(hdlg, INSTABILITY, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, INSTABILITYTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, BLUR)){
			ival = SendDlgItemMessage(hdlg, BLUR, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, BLURTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, FRAMERATELIMIT))
			updateFrameRateLimitSlider(hdlg, FRAMERATELIMIT, FRAMERATELIMITTEXT);
		return TRUE;
    }
    return FALSE;
}


LRESULT screenSaverProc(HWND hwnd, UINT msg, WPARAM wpm, LPARAM lpm){
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
#endif // WIN32
