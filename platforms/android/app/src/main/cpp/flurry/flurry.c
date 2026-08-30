/* -*- Mode: C; tab-width: 4 c-basic-offset: 4 indent-tabs-mode: t -*- */
/*
 * vim: ts=8 sw=4 noet
 */

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

/* flurry */

#if 0
static const char sccsid[] = "@(#)flurry.c	4.07 97/11/24 xlockmore";
#endif

/* Ported to Android as a live wallpaper. The xscreensaver shell that used to be here --
 * xlockmore.h, the XrmOptionDescRec/argtype tables and the ModStruct -- described how to be an
 * xscreensaver hack and nothing about how Flurry works, so it is replaced by the entry points
 * at the bottom of this file. Everything between is Calum Robinson's, unchanged. */

#include <string.h>
#include <unistd.h>
#include <android/log.h>
#include <stdio.h>

#include "flurry.h"

/* Presets, from the upstream -preset option. */
enum {
    FLURRY_PRESET_INSANE = -1,
    FLURRY_PRESET_WATER = 0,
    FLURRY_PRESET_FIRE,
    FLURRY_PRESET_PSYCHEDELIC,
    FLURRY_PRESET_RGB,
    FLURRY_PRESET_BINARY,
    FLURRY_PRESET_CLASSIC,
    FLURRY_PRESET_MAX
};

global_info_t *flurry_info = NULL;


static
double currentTime(void) {
  struct timeval tv;
# ifdef GETTIMEOFDAY_TWO_ARGS
  struct timezone tzp;
  gettimeofday(&tv, &tzp);
# else
  gettimeofday(&tv);
# endif

  return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

double TimeInSecondsSinceStart (const global_info_t *global) {
    return currentTime() - global->gTimeCounter;
}

#if 0
#ifdef __ppc__
static int IsAltiVecAvailable(void)
{
    return 0;
}
#endif
#endif


static
void delete_flurry_info(flurry_info_t *flurry)
{
    int i;

    free(flurry->s);
    free(flurry->star);
    for (i=0;i<MAX_SPARKS;i++)
    {
	free(flurry->spark[i]);
    }
    /* free(flurry); */
}

static
flurry_info_t *new_flurry_info(global_info_t *global, int streams, ColorModes colour, float thickness, float speed, double bf)
{
    int i,k;
    flurry_info_t *flurry = (flurry_info_t *)malloc(sizeof(flurry_info_t));

    if (!flurry) return NULL;

    flurry->flurryRandomSeed = RandFlt(0.0, 300.0);

	flurry->fOldTime = 0;
	flurry->dframe = 0;
	flurry->fTime = TimeInSecondsSinceStart(global) + flurry->flurryRandomSeed;
 	flurry->fDeltaTime = flurry->fTime - flurry->fOldTime;

    flurry->numStreams = streams;
    flurry->streamExpansion = thickness;
    flurry->currentColorMode = colour;
    flurry->briteFactor = bf;

    flurry->s = malloc(sizeof(SmokeV));
    InitSmoke(flurry->s);

    flurry->star = malloc(sizeof(Star));
    InitStar(flurry->star);
    flurry->star->rotSpeed = speed;

    for (i = 0;i < MAX_SPARKS; i++)
    {
	flurry->spark[i] = malloc(sizeof(Spark));
	InitSpark(flurry->spark[i]);
	flurry->spark[i]->mystery = 1800 * (i + 1) / 13; /* 100 * (i + 1) / (flurry->numStreams + 1); */
	UpdateSpark(global, flurry, flurry->spark[i]);
    }

    for (i=0;i<NUMSMOKEPARTICLES/4;i++) {
	for(k=0;k<4;k++) {
	    flurry->s->p[i].dead.i[k] = 1;
	}
    }

    flurry->next = NULL;

    return flurry;
}

static
void GLSetupRC(global_info_t *global)
{
    /* setup the defaults for OpenGL */
    glDisable(GL_DEPTH_TEST);
    glAlphaFunc(GL_GREATER,0.0f);
    glEnable(GL_ALPHA_TEST);
    glShadeModel(GL_FLAT);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);

    glViewport(0,0,(int) global->sys_glWidth,(int) global->sys_glHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0,global->sys_glWidth,0,global->sys_glHeight,-1,1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClear(GL_COLOR_BUFFER_BIT);

    glEnableClientState(GL_COLOR_ARRAY);	
    glEnableClientState(GL_VERTEX_ARRAY);	
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    
#if 0
#ifdef __ppc__
    global->optMode = OPT_MODE_SCALAR_FRSQRTE;

#ifdef __VEC__
    if (IsAltiVecAvailable()) global->optMode = OPT_MODE_VECTOR_UNROLLED;
#endif

#else
    global->optMode = OPT_MODE_SCALAR_BASE;
#endif
#endif /* 0 */
}

static
void GLRenderScene(global_info_t *global, flurry_info_t *flurry, double b)
{
    int i;

    flurry->dframe++;

    flurry->fOldTime = flurry->fTime;
    flurry->fTime = TimeInSecondsSinceStart(global) + flurry->flurryRandomSeed;
    flurry->fDeltaTime = flurry->fTime - flurry->fOldTime;

    flurry->drag = (float) pow(0.9965,flurry->fDeltaTime*85.0);

    UpdateStar(global, flurry, flurry->star);

#ifdef DRAW_SPARKS
    glShadeModel(GL_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE);
#endif

    for (i=0;i<flurry->numStreams;i++) {
	flurry->spark[i]->color[0]=1.0;
	flurry->spark[i]->color[1]=1.0;
	flurry->spark[i]->color[2]=1.0;
	flurry->spark[i]->color[2]=1.0;
	UpdateSpark(global, flurry, flurry->spark[i]);
#ifdef DRAW_SPARKS
	DrawSpark(global, flurry, flurry->spark[i]);
#endif
    }

    switch(global->optMode) {
	case OPT_MODE_SCALAR_BASE:
	    UpdateSmoke_ScalarBase(global, flurry, flurry->s);
	    break;
#if 0
#ifdef __ppc__
	case OPT_MODE_SCALAR_FRSQRTE:
	    UpdateSmoke_ScalarFrsqrte(global, flurry, flurry->s);
	    break;
#endif
#ifdef __VEC__
	case OPT_MODE_VECTOR_SIMPLE:
	    UpdateSmoke_VectorBase(global, flurry, flurry->s);
	    break;
	case OPT_MODE_VECTOR_UNROLLED:
	    UpdateSmoke_VectorUnrolled(global, flurry, flurry->s);
	    break;
#endif
#endif /* 0 */

	default:
	    break;
    }

    /* glDisable(GL_BLEND); */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE);
    glEnable(GL_TEXTURE_2D);

    switch(global->optMode) {
	case OPT_MODE_SCALAR_BASE:
#if 0
#ifdef __ppc__
	case OPT_MODE_SCALAR_FRSQRTE:
#endif
#endif /* 0 */
	    DrawSmoke_Scalar(global, flurry, flurry->s, b);
	    break;
#if 0
#ifdef __VEC__
	case OPT_MODE_VECTOR_SIMPLE:
	case OPT_MODE_VECTOR_UNROLLED:
	    DrawSmoke_Vector(global, flurry, flurry->s, b);
	    break;
#endif
#endif /* 0 */
	default:
	    break;
    }    

    glDisable(GL_TEXTURE_2D);
}

static
void GLResize(global_info_t *global, float w, float h)
{
    global->sys_glWidth = w;
    global->sys_glHeight = h;
}

/* new window size or exposure */
/* ---------------------------------------------------------------------------
 * Android entry points.
 *
 * These stand in for init_flurry / reshape_flurry / draw_flurry / free_flurry. The bodies are
 * the same work those did, minus the ModeInfo plumbing and the GLX calls: the wallpaper's GL
 * thread owns the context and has already made it current before any of these run.
 * ------------------------------------------------------------------------ */

global_info_t *flurry_port_new(int preset)
{
    global_info_t *global = (global_info_t *) calloc(1, sizeof(global_info_t));
    int i;

    if (!global) return NULL;

    global->gTimeCounter = currentTime();
    global->flurry = NULL;
    global->optMode = OPT_MODE_SCALAR_BASE;

    if (preset < FLURRY_PRESET_INSANE || preset >= FLURRY_PRESET_MAX) {
        preset = (int) (random() % FLURRY_PRESET_MAX);
    }

    switch (preset) {
    case FLURRY_PRESET_WATER:
        for (i = 0; i < 9; i++) {
            flurry_info_t *f = new_flurry_info(global, 1, blueColorMode, 100.0, 2.0, 2.0);
            f->next = global->flurry;
            global->flurry = f;
        }
        break;

    case FLURRY_PRESET_FIRE: {
        flurry_info_t *f = new_flurry_info(global, 12, slowCyclicColorMode, 10000.0, 0.2, 1.0);
        f->next = global->flurry;
        global->flurry = f;
        break;
    }

    case FLURRY_PRESET_PSYCHEDELIC: {
        flurry_info_t *f = new_flurry_info(global, 10, rainbowColorMode, 200.0, 2.0, 1.0);
        f->next = global->flurry;
        global->flurry = f;
        break;
    }

    case FLURRY_PRESET_RGB: {
        flurry_info_t *f;
        f = new_flurry_info(global, 3, redColorMode, 100.0, 0.8, 1.0);
        f->next = global->flurry; global->flurry = f;
        f = new_flurry_info(global, 3, greenColorMode, 100.0, 0.8, 1.0);
        f->next = global->flurry; global->flurry = f;
        f = new_flurry_info(global, 3, blueColorMode, 100.0, 0.8, 1.0);
        f->next = global->flurry; global->flurry = f;
        break;
    }

    case FLURRY_PRESET_BINARY: {
        flurry_info_t *f;
        f = new_flurry_info(global, 16, tiedyeColorMode, 1000.0, 0.5, 1.0);
        f->next = global->flurry; global->flurry = f;
        f = new_flurry_info(global, 16, tiedyeColorMode, 1000.0, 1.5, 1.0);
        f->next = global->flurry; global->flurry = f;
        break;
    }

    case FLURRY_PRESET_INSANE: {
        flurry_info_t *f = new_flurry_info(global, 64, tiedyeColorMode, 1000.0, 0.5, 0.5);
        f->next = global->flurry; global->flurry = f;
        break;
    }

    case FLURRY_PRESET_CLASSIC:
    default: {
        flurry_info_t *f = new_flurry_info(global, 5, tiedyeColorMode, 10000.0, 1.0, 1.0);
        f->next = global->flurry;
        global->flurry = f;
        break;
    }
    }

    global->first = 1;
    global->oldFrameTime = -1;
    global->port_clear_frames = 8;
    return global;
}

/*
 * One flurry built from explicit parameters, rather than one of the presets.
 *
 * The presets above are just particular calls to new_flurry_info; there is nothing else to
 * them, so exposing the arguments costs nothing and is the difference between six looks and
 * the whole space. Clamped because the values reach a fixed-size particle pool and a stream
 * count of zero divides by it.
 */
global_info_t *flurry_port_new_custom(int streams, int colour, float thickness,
                                      float speed, float brightness)
{
    global_info_t *global = (global_info_t *) calloc(1, sizeof(global_info_t));
    flurry_info_t *f;

    if (!global) return NULL;

    global->gTimeCounter = currentTime();
    global->flurry = NULL;
    global->optMode = OPT_MODE_SCALAR_BASE;

    if (streams < 1) streams = 1;
    if (streams > 64) streams = 64;
    if (colour < 0 || colour > darkColorMode) colour = tiedyeColorMode;
    if (thickness < 0.1f) thickness = 0.1f;
    if (speed < 0.1f) speed = 0.1f;
    if (brightness < 0.1f) brightness = 0.1f;

    f = new_flurry_info(global, streams, (ColorModes) colour, thickness, speed, brightness);
    if (!f) {
        free(global);
        return NULL;
    }

    f->next = global->flurry;
    global->flurry = f;

    global->first = 1;
    global->oldFrameTime = -1;
    global->port_clear_frames = 8;
    return global;
}

void flurry_port_resize(global_info_t *global, int width, int height)
{
    if (!global || width <= 0 || height <= 0) return;

    GLResize(global, (float) width, (float) height);
    GLSetupRC(global);
}

/* Returns 0 when it deliberately drew nothing, so the caller can tell a skipped frame from a
 * broken one. */
int flurry_port_draw(global_info_t *global)
{
    double newFrameTime, deltaFrameTime = 0, brite;
    GLfloat alpha;
    flurry_info_t *flurry;

    if (!global) return 0;


    newFrameTime = currentTime();

    if (global->oldFrameTime == -1) {
        alpha = 1.0;  /* first frame: clear to black */
    } else {
        /* Upstream's comment, and it still applies: Flurry is designed to run at about this
         * rate, and above it the additive blending saturates and the smoke turns into a white
         * smear. That matters far more on a 120Hz phone than it did on a 2002 desktop, so the
         * cap is kept -- but by returning rather than by usleep, because this runs on the
         * wallpaper's GL thread and sleeping there stalls the whole surface. */
        if (newFrameTime - global->oldFrameTime < 1 / 60.0) {
            return 0;
        }
        deltaFrameTime = newFrameTime - global->oldFrameTime;
        alpha = 5.0 * deltaFrameTime;
    }

    global->oldFrameTime = newFrameTime;
    if (alpha > 0.2) alpha = 0.2;


    if (global->first) {
        global->texid = MakeTexture();
        global->first = 0;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* The buffer starts undefined, so paint it black before the fade has anything coherent to
     * work on. A few frames covers whatever buffers the driver rotates through; after that the
     * fade below keeps the screen dark, and clearing would wipe the trails that are the effect.
     *
     * Alpha is set once here and then masked off for good. Flurry fades with GL_SRC_ALPHA/
     * GL_ONE_MINUS_SRC_ALPHA, which writes destination alpha as well as colour and drags it
     * toward the fade's own value -- harmless for a screensaver that owns the display, wrong for
     * a surface the window compositor blends. RGB is untouched, so the trails are unaffected. */
    if (global->port_clear_frames > 0) {
        global->port_clear_frames--;
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    }

    glColor4f(0.0, 0.0, 0.0, alpha);
    glRectd(0, 0, global->sys_glWidth, global->sys_glHeight);

    brite = pow(deltaFrameTime, 0.75) * 10;

    for (flurry = global->flurry; flurry; flurry = flurry->next) {
        GLRenderScene(global, flurry, brite * flurry->briteFactor);
    }

    return 1;
}

void flurry_port_free(global_info_t *global)
{
    flurry_info_t *flurry, *next;

    if (!global) return;

    for (flurry = global->flurry; flurry; flurry = next) {
        next = flurry->next;
        delete_flurry_info(flurry);
    }

    if (global->texid) glDeleteTextures(1, &global->texid);
    free(global);
}
