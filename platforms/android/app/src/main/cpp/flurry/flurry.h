/*

Copyright (c) 2002, Calum Robinson
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the author nor the names of its contributors may be used
  to endorse or promote products derived from this software without specific
  prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

/* -*- Mode: C; tab-width: 4 c-basic-offset: 4 indent-tabs-mode: t -*- */
/* flurry */
#ifndef __GLCODE__
#define __GLCODE__

/* Ported to Android/GLES2: see flurry_port.h for what these supplied. */
#include "flurry_port.h"

#include <stdlib.h>
#include <math.h>

typedef struct _global_info_t global_info_t;
typedef struct _flurry_info_t flurry_info_t;

#define sqr(X)     ((X) * (X))
#define PI         M_PI
#define DEG2RAD(X) (PI*(X)/180.0)
#define RAD2DEG(X) ((X)*180.0/PI)
#define rnd()      (frand(1.0))

/* fabs: Absolute function. */
/* #undef abs */
/* #define abs(a)     ( (a) > 0 ? (a) : -(a) ) */

/* Force sign clamping to (-1;0;1) */
#define sgn(a)      ((a)<0?-1:((a)?1:0))

/* used to compute the min and max of two expresions */
#define MIN_(a, b)  (((a) < (b)) ? (a) : (b)) 
#define MAX_(a, b)  (((a) > (b)) ? (a) : (b)) 

typedef union {
    float		f[4];
#if 0
#if __VEC__
    vector float	v;
#endif
#endif /* 0 */
} floatToVector;

typedef union {
    unsigned int	i[4];
#if 0
#if __VEC__
    vector unsigned int	v;
#endif
#endif /* 0 */
} intToVector;

typedef struct SmokeParticleV  
{
	floatToVector color[4];
	floatToVector position[3];
	floatToVector oldposition[3];
	floatToVector delta[3];
	intToVector dead;
	floatToVector time;
	intToVector animFrame;
} SmokeParticleV;

#define NUMSMOKEPARTICLES 3600

typedef struct SmokeV  
{
	SmokeParticleV p[NUMSMOKEPARTICLES/4];
	int nextParticle;
        int nextSubParticle;
	float lastParticleTime;
	int firstTime;
	long frame;
	float old[3];
        floatToVector seraphimVertices[NUMSMOKEPARTICLES*2+1];
        floatToVector seraphimColors[NUMSMOKEPARTICLES*4+1];
	float seraphimTextures[NUMSMOKEPARTICLES*2*4];
} SmokeV;

void InitSmoke(SmokeV *s);

void UpdateSmoke_ScalarBase(global_info_t *global, flurry_info_t *flurry, SmokeV *s);
#if 0
#ifdef __ppc__
void UpdateSmoke_ScalarFrsqrte(global_info_t *global, flurry_info_t *flurry, SmokeV *s);
#endif
#ifdef __VEC__
void UpdateSmoke_VectorBase(global_info_t *global, flurry_info_t *flurry, SmokeV *s);
void UpdateSmoke_VectorUnrolled(global_info_t *global, flurry_info_t *flurry, SmokeV *s);
#endif
#endif /* 0 */

void DrawSmoke_Scalar(global_info_t *global, flurry_info_t *flurry, SmokeV *s, float);
void DrawSmoke_Vector(global_info_t *global, flurry_info_t *flurry, SmokeV *s, float);

typedef struct Star  
{
	float position[3];
	float mystery;
	float rotSpeed;
	int ate;
} Star;

void UpdateStar(global_info_t *global, flurry_info_t *flurry, Star *s);
void InitStar(Star *s);

typedef struct Spark  
{
    float position[3];
    int mystery;
    float delta[3];
    float color[4];    
} Spark;

void UpdateSparkColour(global_info_t *info, flurry_info_t *flurry, Spark *s);
void InitSpark(Spark *s);
void UpdateSpark(global_info_t *info, flurry_info_t *flurry, Spark *s);
void DrawSpark(global_info_t *info, flurry_info_t *flurry, Spark *s);

/* #define FastDistance2D(x,y) hypot(x,y) */

/* UInt8  sys_glBPP=32; */
/* int SSMODE = FALSE; */
/* int currentVideoMode = 0; */
/* int cohesiveness = 7; */
/* int fieldStrength; */
/* int colorCoherence = 7; */
/* int fieldIncoherence = 0; */
/* int ifieldSpeed = 120; */

static inline float FastDistance2D(float x, float y)
{
        /* this function computes the distance from 0,0 to x,y with ~3.5% error */
	float mn;
	/* first compute the absolute value of x,y */
	x = (x < 0.0f) ? -x : x;
	y = (y < 0.0f) ? -y : y;
	
	/* compute the minimum of x,y */
	mn = x<y?x:y;
	
	/* return the distance */
	return(x+y-(mn*0.5f)-(mn*0.25f)+(mn*0.0625f));
}

#if 0
#ifdef __VEC__

static vector float FastDistance2DV(vector float x, vector float y) {
    vector float mn, temp;
    
    x = vec_abs(x);
    y = vec_abs(y);
    mn = vec_min(x,y);
    temp = vec_add(x,y);
    temp = vec_madd(mn, (vector float)(-0.6875), temp);
    return temp;
}

#endif
#endif /* 0 */

#define RandFlt(min, max) ((min) + frand((max) - (min)))

#define RandBell(scale) ((scale) * (-(frand(.5) + frand(.5) + frand(.5))))

extern GLuint theTexture;

GLuint MakeTexture(void);

#define OPT_MODE_SCALAR_BASE		0x0

#if 0
#ifdef __ppc__
#define OPT_MODE_SCALAR_FRSQRTE		0x1
#endif

#ifdef __VEC__
#define OPT_MODE_VECTOR_SIMPLE		0x2
#define OPT_MODE_VECTOR_UNROLLED	0x3
#endif
#endif /* 0 */

typedef enum _ColorModes
{
	redColorMode = 0,
	magentaColorMode,
	blueColorMode,
	cyanColorMode,
	greenColorMode,
	yellowColorMode,
	slowCyclicColorMode,
	cyclicColorMode,
	tiedyeColorMode,
	rainbowColorMode,
	whiteColorMode,
	multiColorMode,
	darkColorMode
} ColorModes;

#define gravity 1500000.0f

#define incohesion 0.07f
#define colorIncoherence 0.15f
#define streamSpeed 450.0
#define fieldCoherence 0
#define fieldSpeed 12.0f
#define numParticles 250
#define starSpeed 50
#define seraphDistance 2000.0f
#define streamSize 25000.0f
#define fieldRange 1000.0f
#define streamBias 7.0f

#define MAX_SPARKS 64

struct _flurry_info_t {
	flurry_info_t *next;
	ColorModes currentColorMode;
	SmokeV *s;
	Star *star;
	Spark *spark[MAX_SPARKS];
	float streamExpansion;
	int numStreams;
	double flurryRandomSeed;
	double fTime;
	double fOldTime;
	double fDeltaTime;
	double briteFactor;
	float drag;
	int dframe;
};

struct _global_info_t {
  /* system values */
        int optMode;

        /* Port-only. Flurry never clears: it fades the screen by a few percent each frame, which
         * is how the trails exist. That assumes it owns a buffer that started black. Here it gets
         * a rotating set of swapchain buffers full of uninitialised GPU memory, and an 8% fade
         * cannot win against that -- it shows as static the smoke sits behind. Clear each buffer
         * once, then hand over to the fade. */
        int port_clear_frames;

	float sys_glWidth;
	float sys_glHeight;

	double gTimeCounter;
	int first;
	double oldFrameTime;

	flurry_info_t *flurry;
        GLuint texid;
};

#define kNumSpectrumEntries 512

double TimeInSecondsSinceStart(const global_info_t *global);

#endif /* Include/Define */
