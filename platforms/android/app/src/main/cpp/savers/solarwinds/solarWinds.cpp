/*
 * Copyright (C) 1999-2010  Terence M. Welsh
 *
 * This file is part of Solar Winds.
 *
 * Solar Winds is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Solar Winds is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

// Solar Winds screen saver

#ifdef WIN32
#include <windows.h>
#include <rsWin32Saver/rsWin32Saver.h>
#include <regstr.h>
#include <commctrl.h>
#include "resource.h"
#endif
#ifdef RS_XSCREENSAVER
#include <rsXScreenSaver/rsXScreenSaver.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <rsText/rsText.h>
#include <rsMath/rsMath.h>
#include <math.h>
#include <time.h>
#include <GL/gl.h>
#include <GL/glu.h>


#define NUMCONSTS 9
#define PIx2 6.28318530718f
#define DEG2RAD 0.0174532925f
#define LIGHTSIZE 64


class wind;


// Global variables
#ifdef WIN32
LPCTSTR registryPath = ("Software\\Really Slick\\Solar Winds");
HGLRC hglrc;
HDC hdc;
#endif
float aspectRatio;
float frameTime = 0.0f;
int readyToDraw = 0;
wind *winds;
float lumdiff;
unsigned char lightTexture[LIGHTSIZE][LIGHTSIZE];
// text output
rsText* textwriter;
// Parameters edited in the dialog box
int dWinds;
int dEmitters;
int dParticles;
int dGeometry;
int dSize;
int dParticlespeed;
int dEmitterspeed;
int dWindspeed;
int dBlur;
#ifdef RS_XSCREENSAVER
enum{
	DEFAULTS1,
	DEFAULTS2,
	DEFAULTS3,
	DEFAULTS4,
	DEFAULTS5,
	DEFAULTS6
};
#endif


class wind{
public:
	float **emitters;
	float **particles;
	int **linelist;
	int *lastparticle;
	int whichparticle;
	float c[NUMCONSTS];
	float ct[NUMCONSTS];
	float cv[NUMCONSTS];
	int numEmitters;
	int numParticles;
	int hasLineList;

	wind();
	~wind();
	void update();
};

wind::wind(){
	int i;

	emitters = new float*[dEmitters];
	for(i=0; i<dEmitters; i++){
		emitters[i] = new float[3];
		emitters[i][0] = rsRandf(60.0f) - 30.0f;
		emitters[i][1] = rsRandf(60.0f) - 30.0f;
		emitters[i][2] = rsRandf(30.0f) - 15.0f;
	}

	particles = new float*[dParticles];
	for(i=0; i<dParticles; i++){
		particles[i] = new float[6];  // 3 for pos, 3 for color
		particles[i][2] = 100.0f;  // start particles behind viewer
	}

	whichparticle = 0;

	if(dGeometry == 2){  // allocate memory for lines
		linelist = new int*[dParticles];
		for(i=0; i<dParticles; i++){
			linelist[i] = new int[2];
			linelist[i][0] = -1;
			linelist[i][1] = -1;
		}
		lastparticle = new int[dEmitters];
		for(i=0; i<dEmitters; i++)
			lastparticle[i] = i;
	}

	for(i=0; i<NUMCONSTS; i++){
		ct[i] = rsRandf(PIx2);
		cv[i] = rsRandf(0.00005f * float(dWindspeed) * float(dWindspeed))
			+ 0.00001f * float(dWindspeed) * float(dWindspeed);
	}

	numEmitters = dEmitters;
	numParticles = dParticles;
	hasLineList = (dGeometry == 2);
}

wind::~wind(){
	int i;

	for(i=0; i<numEmitters; i++)
		delete[] emitters[i];
	delete[] emitters;

	for(i=0; i<numParticles; i++)
		delete[] particles[i];
	delete[] particles;

	if(hasLineList){
		for(i=0; i<numParticles; i++)
			delete[] linelist[i];
		delete[] linelist;
		delete[] lastparticle;
	}
}

void wind::update(){
	int i;
	float x, y, z;
	float temp;
	static float evel = float(dEmitterspeed) * 0.01f;
	static float pvel = float(dParticlespeed) * 0.01f;
	static float pointsize = 0.04f * float(dSize);
	static float linesize = 0.005f * float(dSize);

	// update constants
	for(i=0; i<NUMCONSTS; i++){
		ct[i] += cv[i];
		if(ct[i] > PIx2)
			ct[i] -= PIx2;
		c[i] = cosf(ct[i]);
	}
	
	// calculate emissions
	for(i=0; i<dEmitters; i++){
		emitters[i][2] += evel;  // emitter moves toward viewer
		if(emitters[i][2] > 15.0f){  // reset emitter
			emitters[i][0] = rsRandf(60.0f) - 30.0f;
			emitters[i][1] = rsRandf(60.0f) - 30.0f;
			emitters[i][2] = -15.0f;
		}
		particles[whichparticle][0] = emitters[i][0];
		particles[whichparticle][1] = emitters[i][1];
		particles[whichparticle][2] = emitters[i][2];
		if(dGeometry == 2){  // link particles to form lines
			if(linelist[whichparticle][0] >= 0)
				linelist[linelist[whichparticle][0]][1] = -1;
			linelist[whichparticle][0] = -1;
			if(emitters[i][2] == -15.0f)
				linelist[whichparticle][1] = -1;
			else
				linelist[whichparticle][1] = lastparticle[i];
			linelist[lastparticle[i]][0] = whichparticle;
			lastparticle[i] = whichparticle;
		}
		whichparticle++;
		if(whichparticle >= dParticles)
			whichparticle = 0;
	}

	// calculate particle positions and colors
	// first modify constants that affect colors
	c[6] *= 9.0f / float(dParticlespeed);
	c[7] *= 9.0f / float(dParticlespeed);
	c[8] *= 9.0f / float(dParticlespeed);
	// then update each particle
	for(i=0; i<dParticles; i++){
		// store old positions
		x = particles[i][0];
		y = particles[i][1];
		z = particles[i][2];
		// make new positions
		particles[i][0] = x + (c[0] * y + c[1] * z) * pvel;
		particles[i][1] = y + (c[2] * z + c[3] * x) * pvel;
		particles[i][2] = z + (c[4] * x + c[5] * y) * pvel;
		// calculate colors
		particles[i][3] = float(fabs((particles[i][0] - x) * c[6]));
		particles[i][4] = float(fabs((particles[i][1] - y) * c[7]));
		particles[i][5] = float(fabs((particles[i][2] - z) * c[8]));
		// clamp colors
		if(particles[i][3] > 1.0f)
			particles[i][3] = 1.0f;
		if(particles[i][4] > 1.0f)
			particles[i][4] = 1.0f;
		if(particles[i][5] > 1.0f)
			particles[i][5] = 1.0f;
	}

	// draw particles
	switch(dGeometry){
	case 0:  // lights
		for(i=0; i<dParticles; i++){
			glColor3fv(&particles[i][3]);
			glPushMatrix();
				glTranslatef(particles[i][0], particles[i][1], particles[i][2]);
				glCallList(1);
			glPopMatrix();
		}
		break;
	case 1:  // points
		for(i=0; i<dParticles; i++){
			temp = particles[i][2] + 40.0f;
			if(temp < 0.01f)
				temp = 0.01f;
			glPointSize(pointsize * temp);
			glBegin(GL_POINTS);
				glColor3fv(&particles[i][3]);
				glVertex3fv(particles[i]);
			glEnd();
		}
		break;
	case 2:  // lines
		for(i=0; i<dParticles; i++){
			temp = particles[i][2] + 40.0f;
			if(temp < 0.01f)
				temp = 0.01f;
			if(linelist[i][1] >= 0){
				glLineWidth(linesize * temp);
				glBegin(GL_LINES);
					glColor3fv(&particles[i][3]);
					if(linelist[i][0] == -1)
						glColor3f(0.0f, 0.0f, 0.0f);
					glVertex3fv(particles[i]);
					glColor3fv(&particles[linelist[i][1]][3]);
					if(linelist[linelist[i][1]][1] == -1)
						glColor3f(0.0f, 0.0f, 0.0f);
					glVertex3fv(particles[linelist[i][1]]);
				glEnd();
			}
		}
	}
}


void draw(){
	int i;

	if(!dBlur)
		glClear(GL_COLOR_BUFFER_BIT);
	else{
		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
			glLoadIdentity();
			glOrtho(0.0, 1.0, 0.0, 1.0, 1.0, -1.0);
			glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glColor4f(0.0f, 0.0f, 0.0f, 0.5f - (float(dBlur) * 0.0049f));
				glBegin(GL_TRIANGLE_STRIP);
					glVertex3f(0.0f, 0.0f, 0.0f);
					glVertex3f(1.0f, 0.0f, 0.0f);
					glVertex3f(0.0f, 1.0f, 0.0f);
					glVertex3f(1.0f, 1.0f, 0.0f);
				glEnd();
				if(dGeometry == 0)
					glBlendFunc(GL_ONE, GL_ONE);
				else
					glBlendFunc(GL_SRC_ALPHA, GL_ONE);  // Necessary for point and line smoothing (I don't know why)
						// Maybe it's just my video card...
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
	}

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0, 0.0, -15.0);

	// You should need to draw twice if using blur, once to each buffer.
	// But wglSwapLayerBuffers appears to copy the back to the
	// front instead of just switching the pointers to them.  It turns
	// out that both NVidia and 3dfx prefer to use PFD_SWAP_COPY instead
	// of PFD_SWAP_EXCHANGE in the PIXELFORMATDESCRIPTOR.  I don't know why...
	// So this may not work right on other platforms or all video cards.

	// Update surfaces
	for(i=0; i<dWinds; i++)
		winds[i].update();

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
		dWinds = 1;
		dEmitters = 30;
		dParticles = 2000;
		dGeometry = 0;
		dSize = 50;
		dWindspeed = 20;
		dEmitterspeed = 15;
		dParticlespeed = 10;
		dBlur = 40;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS2:  // Cosmic Strings
		dWinds = 1;
		dEmitters = 50;
		dParticles = 3000;
		dGeometry = 2;
		dSize = 20;
		dWindspeed = 10;
		dEmitterspeed = 10;
		dParticlespeed = 10;
		dBlur = 10;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS3:  // Cold Pricklies
		dWinds = 1;
		dEmitters = 300;
		dParticles = 3000;
		dGeometry = 2;
		dSize = 5;
		dWindspeed = 20;
		dEmitterspeed = 100;
		dParticlespeed = 15;
		dBlur = 70;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS4:  // Space Fur
		dWinds = 2;
		dEmitters = 400;
		dParticles = 1600;
		dGeometry = 2;
		dSize = 15;
		dWindspeed = 20;
		dEmitterspeed = 15;
		dParticlespeed = 10;
		dBlur = 0;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS5:  // Jiggly
		dWinds = 1;
		dEmitters = 40;
		dParticles = 1200;
		dGeometry = 1;
		dSize = 20;
		dWindspeed = 100;
		dEmitterspeed = 20;
		dParticlespeed = 4;
		dBlur = 50;
		dFrameRateLimit = 60;
		break;
	case DEFAULTS6:  // Undertow
		dWinds = 1;
		dEmitters = 400;
		dParticles = 1200;
		dGeometry = 0;
		dSize = 40;
		dWindspeed = 20;
		dEmitterspeed = 1;
		dParticlespeed = 100;
		dBlur = 50;
		dFrameRateLimit = 60;
	}
}


#ifdef RS_XSCREENSAVER
void handleCommandLine(int argc, char* argv[]){
	setDefaults(DEFAULTS1);
	getArgumentsValue(argc, argv, std::string("-winds"), dWinds, 1, 10);
	getArgumentsValue(argc, argv, std::string("-emitters"), dEmitters, 1, 1000);
	getArgumentsValue(argc, argv, std::string("-perticles"), dParticles, 1, 10000);
	getArgumentsValue(argc, argv, std::string("-geometry"), dGeometry, 0, 2);
	getArgumentsValue(argc, argv, std::string("-size"), dSize, 1, 100);
	getArgumentsValue(argc, argv, std::string("-windspeed"), dWindspeed, 1, 100);
	getArgumentsValue(argc, argv, std::string("-emitterspeed"), dEmitterspeed, 1, 100);
	getArgumentsValue(argc, argv, std::string("-particlespeed"), dParticlespeed, 1, 100);
	getArgumentsValue(argc, argv, std::string("-blur"), dBlur, 1, 100);
}
#endif


void reshape(int width, int height){
	glViewport(0, 0, width, height);
	aspectRatio = float(width) / float(height);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(90.0, aspectRatio, 1.0, 10000.0);
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

	reshape(rect.right, rect.bottom);
#endif
#ifdef RS_XSCREENSAVER
void initSaver(){
#endif
	int i, j;
	float x, y, temp;

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if(!dGeometry)
		glBlendFunc(GL_ONE, GL_ONE);
	else
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);  // Necessary for point and line smoothing (I don't know why)
	glEnable(GL_BLEND);

	if(!dGeometry){  // Init lights
		for(i=0; i<LIGHTSIZE; i++){
			for(j=0; j<LIGHTSIZE; j++){
				x = float(i - LIGHTSIZE / 2) / float(LIGHTSIZE / 2);
				y = float(j - LIGHTSIZE / 2) / float(LIGHTSIZE / 2);
				temp = 1.0f - float(sqrt((x * x) + (y * y)));
				if(temp > 1.0f)
					temp = 1.0f;
				if(temp < 0.0f)
					temp = 0.0f;
				lightTexture[i][j] = (unsigned char)(255.0f * temp);
			}
		}
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 1);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, 1, LIGHTSIZE, LIGHTSIZE, 0,
			GL_LUMINANCE, GL_UNSIGNED_BYTE, lightTexture);
		temp = 0.02f * float(dSize);
		glNewList(1, GL_COMPILE);
			glBindTexture(GL_TEXTURE_2D, 1);
			glBegin(GL_TRIANGLE_STRIP);
				glTexCoord2f(0.0f, 0.0f);
				glVertex3f(-temp, -temp, 0.0f);
				glTexCoord2f(1.0f, 0.0f);
				glVertex3f(temp, -temp, 0.0f);
				glTexCoord2f(0.0f, 1.0f);
				glVertex3f(-temp, temp, 0.0f);
				glTexCoord2f(1.0f, 1.0f);
				glVertex3f(temp, temp, 0.0f);
			glEnd();
		glEndList();
	}

	if(dGeometry == 1){  // init point smoothing
		glEnable(GL_POINT_SMOOTH);
		glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
	}

	if(dGeometry == 2){  // init line smoothing
		glEnable(GL_LINE_SMOOTH);
		glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
	}
	// Initialize surfaces
	winds = new wind[dWinds];

	// Initialize text
	textwriter = new rsText;

	readyToDraw = 1;
}


#ifdef RS_XSCREENSAVER
void cleanUp(){
	// Free memory
	delete[] winds;
}
#endif


#ifdef WIN32
void cleanUp(HWND hwnd){
	// Free memory
	delete[] winds;

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

	result = RegQueryValueEx(skey, "Winds", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dWinds = val;
	result = RegQueryValueEx(skey, "Emitters", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dEmitters = val;
	result = RegQueryValueEx(skey, "Particles", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dParticles = val;
	result = RegQueryValueEx(skey, "Geometry", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dGeometry = val;
	result = RegQueryValueEx(skey, "Size", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dSize = val;
	result = RegQueryValueEx(skey, "Windspeed", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dWindspeed = val;
	result = RegQueryValueEx(skey, "Emitterspeed", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dEmitterspeed = val;
	result = RegQueryValueEx(skey, "Particlespeed", 0, &valtype, (LPBYTE)&val, &valsize);
	if(result == ERROR_SUCCESS)
		dParticlespeed = val;
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

	val = dWinds;
	RegSetValueEx(skey, "Winds", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dEmitters;
	RegSetValueEx(skey, "Emitters", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dParticles;
	RegSetValueEx(skey, "Particles", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dGeometry;
	RegSetValueEx(skey, "Geometry", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dSize;
	RegSetValueEx(skey, "Size", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dWindspeed;
	RegSetValueEx(skey, "Windspeed", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dEmitterspeed;
	RegSetValueEx(skey, "Emitterspeed", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dParticlespeed;
	RegSetValueEx(skey, "Particlespeed", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dBlur;
	RegSetValueEx(skey, "Blur", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));
	val = dFrameRateLimit;
	RegSetValueEx(skey, "FrameRateLimit", 0, REG_DWORD, (CONST BYTE*)&val, sizeof(val));

	RegCloseKey(skey);
}


INT_PTR CALLBACK aboutProc(HWND hdlg, UINT msg, WPARAM wpm, LPARAM lpm){
	switch(msg){
	case WM_CTLCOLORSTATIC:
		if((HWND(lpm) == GetDlgItem(hdlg, WEBPAGE)) || (HWND(lpm) == GetDlgItem(hdlg, MAIL))){
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
			ShellExecute(NULL, "open", "http://www.reallyslick.com", NULL, NULL, SW_SHOWNORMAL);
		}
	}
	return FALSE;
}


void initControls(HWND hdlg){
	char cval[16];

	SendDlgItemMessage(hdlg, WINDS, UDM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(10), DWORD(1))));
	SendDlgItemMessage(hdlg, WINDS, UDM_SETPOS, 0, LPARAM(dWinds));

	SendDlgItemMessage(hdlg, EMITTERS, UDM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1000), DWORD(1))));
	SendDlgItemMessage(hdlg, EMITTERS, UDM_SETPOS, 0, LPARAM(dEmitters));

	SendDlgItemMessage(hdlg, PARTICLES, UDM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(10000), DWORD(1))));
	SendDlgItemMessage(hdlg, PARTICLES, UDM_SETPOS, 0, LPARAM(dParticles));

	SendDlgItemMessage(hdlg, GEOMETRY, CB_DELETESTRING, WPARAM(2), 0);
	SendDlgItemMessage(hdlg, GEOMETRY, CB_DELETESTRING, WPARAM(1), 0);
	SendDlgItemMessage(hdlg, GEOMETRY, CB_DELETESTRING, WPARAM(0), 0);
	SendDlgItemMessage(hdlg, GEOMETRY, CB_ADDSTRING, 0, LPARAM("Lights"));
	SendDlgItemMessage(hdlg, GEOMETRY, CB_ADDSTRING, 0, LPARAM("Points"));
	SendDlgItemMessage(hdlg, GEOMETRY, CB_ADDSTRING, 0, LPARAM("Lines"));
	SendDlgItemMessage(hdlg, GEOMETRY, CB_SETCURSEL, WPARAM(dGeometry), 0);

	SendDlgItemMessage(hdlg, SIZE, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(100))));
	SendDlgItemMessage(hdlg, SIZE, TBM_SETPOS, 1, LPARAM(dSize));
	SendDlgItemMessage(hdlg, SIZE, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, SIZE, TBM_SETPAGESIZE, 0, LPARAM(10));
	sprintf_s(cval, "%d", dSize);
	SendDlgItemMessage(hdlg, SIZETEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, WINDSPEED, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(100))));
	SendDlgItemMessage(hdlg, WINDSPEED, TBM_SETPOS, 1, LPARAM(dWindspeed));
	SendDlgItemMessage(hdlg, WINDSPEED, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, WINDSPEED, TBM_SETPAGESIZE, 0, LPARAM(10));
	sprintf_s(cval, "%d", dWindspeed);
	SendDlgItemMessage(hdlg, WINDSPEEDTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, EMITTERSPEED, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(100))));
	SendDlgItemMessage(hdlg, EMITTERSPEED, TBM_SETPOS, 1, LPARAM(dEmitterspeed));
	SendDlgItemMessage(hdlg, EMITTERSPEED, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, EMITTERSPEED, TBM_SETPAGESIZE, 0, LPARAM(10));
	sprintf_s(cval, "%d", dEmitterspeed);
	SendDlgItemMessage(hdlg, EMITTERSPEEDTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, PARTICLESPEED, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(1), DWORD(100))));
	SendDlgItemMessage(hdlg, PARTICLESPEED, TBM_SETPOS, 1, LPARAM(dParticlespeed));
	SendDlgItemMessage(hdlg, PARTICLESPEED, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, PARTICLESPEED, TBM_SETPAGESIZE, 0, LPARAM(10));
	sprintf_s(cval, "%d", dParticlespeed);
	SendDlgItemMessage(hdlg, PARTICLESPEEDTEXT, WM_SETTEXT, 0, LPARAM(cval));

	SendDlgItemMessage(hdlg, BLUR, TBM_SETRANGE, 0, LPARAM(MAKELONG(DWORD(0), DWORD(100))));
	SendDlgItemMessage(hdlg, BLUR, TBM_SETPOS, 1, LPARAM(dBlur));
	SendDlgItemMessage(hdlg, BLUR, TBM_SETLINESIZE, 0, LPARAM(1));
	SendDlgItemMessage(hdlg, BLUR, TBM_SETPAGESIZE, 0, LPARAM(10));
	sprintf_s(cval, "%d", dBlur);
	SendDlgItemMessage(hdlg, BLURTEXT, WM_SETTEXT, 0, LPARAM(cval));

	initFrameRateLimitSlider(hdlg, FRAMERATELIMIT, FRAMERATELIMITTEXT);
}


INT_PTR CALLBACK screenSaverConfigureDialog(HWND hdlg, UINT msg,
										 WPARAM wpm, LPARAM lpm){
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
            dWinds = SendDlgItemMessage(hdlg, WINDS, UDM_GETPOS, 0, 0);
			dEmitters = SendDlgItemMessage(hdlg, EMITTERS, UDM_GETPOS, 0, 0);
			dParticles = SendDlgItemMessage(hdlg, PARTICLES, UDM_GETPOS, 0, 0);
			dGeometry = SendDlgItemMessage(hdlg, GEOMETRY, CB_GETCURSEL, 0, 0);
			dSize = SendDlgItemMessage(hdlg, SIZE, TBM_GETPOS, 0, 0);
			dWindspeed = SendDlgItemMessage(hdlg, WINDSPEED, TBM_GETPOS, 0, 0);
			dEmitterspeed = SendDlgItemMessage(hdlg, EMITTERSPEED, TBM_GETPOS, 0, 0);
			dParticlespeed = SendDlgItemMessage(hdlg, PARTICLESPEED, TBM_GETPOS, 0, 0);
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
		}
        return TRUE;
	case WM_HSCROLL:
		if(HWND(lpm) == GetDlgItem(hdlg, SIZE)){
			ival = SendDlgItemMessage(hdlg, SIZE, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, SIZETEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, WINDSPEED)){
			ival = SendDlgItemMessage(hdlg, WINDSPEED, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, WINDSPEEDTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, EMITTERSPEED)){
			ival = SendDlgItemMessage(hdlg, EMITTERSPEED, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, EMITTERSPEEDTEXT, WM_SETTEXT, 0, LPARAM(cval));
		}
		if(HWND(lpm) == GetDlgItem(hdlg, PARTICLESPEED)){
			ival = SendDlgItemMessage(hdlg, PARTICLESPEED, TBM_GETPOS, 0, 0);
			sprintf_s(cval, "%d", ival);
			SendDlgItemMessage(hdlg, PARTICLESPEEDTEXT, WM_SETTEXT, 0, LPARAM(cval));
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


LONG screenSaverProc(HWND hwnd, UINT msg, WPARAM wpm, LPARAM lpm){
	static unsigned long threadID;

	switch(msg){
	case WM_CREATE:
		readRegistry();
		initSaver(hwnd);
		readyToDraw = 1;
		break;
	case WM_DESTROY:
		readyToDraw = 0;
		cleanUp(hwnd);
		break;
	}
	return defScreenSaverProc(hwnd, msg, wpm, lpm);
}
#endif
