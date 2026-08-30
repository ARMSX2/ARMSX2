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

/* Smoke.cpp: implementation of the Smoke class. */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "flurry.h"

#define MAXANGLES 16384
#define NOT_QUITE_DEAD 3

#define intensity 75000.0f;

void InitSmoke(SmokeV *s)
{
    int i;
    s->nextParticle = 0;
    s->nextSubParticle = 0;
    s->lastParticleTime = 0.25f;
    s->firstTime = 1;
    s->frame = 0;
    for (i=0;i<3;i++) {
        s->old[i] = RandFlt(-100.0, 100.0);
    }
}

void UpdateSmoke_ScalarBase(global_info_t *global, flurry_info_t *flurry, SmokeV *s)
{
    int i,j,k;
    float sx = flurry->star->position[0];
    float sy = flurry->star->position[1];
    float sz = flurry->star->position[2];
    double frameRate;
    double frameRateModifier;


    s->frame++;

    if(!s->firstTime) {
        /* release 12 puffs every frame */
        if(flurry->fTime - s->lastParticleTime >= 1.0f / 121.0f) {
            float dx,dy,dz,deltax,deltay,deltaz;
            float f;
            float rsquared;
            float mag;

            dx = s->old[0] - sx;
            dy = s->old[1] - sy;
            dz = s->old[2] - sz;
            mag = 5.0f;
            deltax = (dx * mag);
            deltay = (dy * mag);
            deltaz = (dz * mag);
            for(i=0;i<flurry->numStreams;i++) {
                float streamSpeedCoherenceFactor;
                
                s->p[s->nextParticle].delta[0].f[s->nextSubParticle] = deltax;
                s->p[s->nextParticle].delta[1].f[s->nextSubParticle] = deltay;
                s->p[s->nextParticle].delta[2].f[s->nextSubParticle] = deltaz;
                s->p[s->nextParticle].position[0].f[s->nextSubParticle] = sx;
                s->p[s->nextParticle].position[1].f[s->nextSubParticle] = sy;
                s->p[s->nextParticle].position[2].f[s->nextSubParticle] = sz;
                s->p[s->nextParticle].oldposition[0].f[s->nextSubParticle] = sx;
                s->p[s->nextParticle].oldposition[1].f[s->nextSubParticle] = sy;
                s->p[s->nextParticle].oldposition[2].f[s->nextSubParticle] = sz;
                streamSpeedCoherenceFactor = MAX_(0.0f,1.0f + RandBell(0.25f*incohesion));
                dx = s->p[s->nextParticle].position[0].f[s->nextSubParticle] - flurry->spark[i]->position[0];
                dy = s->p[s->nextParticle].position[1].f[s->nextSubParticle] - flurry->spark[i]->position[1];
                dz = s->p[s->nextParticle].position[2].f[s->nextSubParticle] - flurry->spark[i]->position[2];
                rsquared = (dx*dx+dy*dy+dz*dz);
                f = streamSpeed * streamSpeedCoherenceFactor;

                mag = f / (float) sqrt(rsquared);

                s->p[s->nextParticle].delta[0].f[s->nextSubParticle] -= (dx * mag);
                s->p[s->nextParticle].delta[1].f[s->nextSubParticle] -= (dy * mag);
                s->p[s->nextParticle].delta[2].f[s->nextSubParticle] -= (dz * mag);
                s->p[s->nextParticle].color[0].f[s->nextSubParticle] = flurry->spark[i]->color[0] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[1].f[s->nextSubParticle] = flurry->spark[i]->color[1] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[2].f[s->nextSubParticle] = flurry->spark[i]->color[2] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[3].f[s->nextSubParticle] = 0.85f * (1.0f + RandBell(0.5f*colorIncoherence));
                s->p[s->nextParticle].time.f[s->nextSubParticle] = flurry->fTime;
                s->p[s->nextParticle].dead.i[s->nextSubParticle] = 0;
                s->p[s->nextParticle].animFrame.i[s->nextSubParticle] = random()&63;
                s->nextSubParticle++;
                if (s->nextSubParticle==4) {
                    s->nextParticle++;
                    s->nextSubParticle=0;
                }
                if (s->nextParticle >= NUMSMOKEPARTICLES/4) {
                    s->nextParticle = 0;
                    s->nextSubParticle = 0;
                }
            }

            s->lastParticleTime = flurry->fTime;
        }
    } else {
        s->lastParticleTime = flurry->fTime;
        s->firstTime = 0;
    }

    for(i=0;i<3;i++) {
        s->old[i] = flurry->star->position[i];
    }
    
    frameRate = ((double) flurry->dframe)/(flurry->fTime);
    frameRateModifier = 42.5f / frameRate;

    for(i=0;i<NUMSMOKEPARTICLES/4;i++) {        
        for(k=0; k<4; k++) {
            float dx,dy,dz;
            float f;
            float rsquared;
            float mag;
            float deltax;
            float deltay;
            float deltaz;
        
            if (s->p[i].dead.i[k]) {
                continue;
            }
            
            deltax = s->p[i].delta[0].f[k];
            deltay = s->p[i].delta[1].f[k];
            deltaz = s->p[i].delta[2].f[k];
            
            for(j=0;j<flurry->numStreams;j++) {
                dx = s->p[i].position[0].f[k] - flurry->spark[j]->position[0];
                dy = s->p[i].position[1].f[k] - flurry->spark[j]->position[1];
                dz = s->p[i].position[2].f[k] - flurry->spark[j]->position[2];
                rsquared = (dx*dx+dy*dy+dz*dz);

                f = (gravity/rsquared) * frameRateModifier;

                if ((((i*4)+k) % flurry->numStreams) == j) {
                    f *= 1.0f + streamBias;
                }
                mag = f / (float) sqrt(rsquared);
                
                deltax -= (dx * mag);
                deltay -= (dy * mag);
                deltaz -= (dz * mag);
            }
    
            /* slow this particle down by flurry->drag */
            deltax *= flurry->drag;
            deltay *= flurry->drag;
            deltaz *= flurry->drag;
            
            if((deltax*deltax+deltay*deltay+deltaz*deltaz) >= 25000000.0f) {
                s->p[i].dead.i[k] = 1;
                continue;
            }
    
            /* update the position */
            s->p[i].delta[0].f[k] = deltax;
            s->p[i].delta[1].f[k] = deltay;
            s->p[i].delta[2].f[k] = deltaz;
            for(j=0;j<3;j++) {
                s->p[i].oldposition[j].f[k] = s->p[i].position[j].f[k];
                s->p[i].position[j].f[k] += (s->p[i].delta[j].f[k])*flurry->fDeltaTime;
            }
        }
    }
}

#if 0
#ifdef __ppc__

void UpdateSmoke_ScalarFrsqrte(global_info_t *global, flurry_info_t *flurry, SmokeV *s)
{
    int i,j,k;
    float sx = flurry->star->position[0];
    float sy = flurry->star->position[1];
    float sz = flurry->star->position[2];
    double frameRate;
    double frameRateModifier;


    s->frame++;

    if(!s->firstTime) {
        /* release 12 puffs every frame */
        if(flurry->fTime - s->lastParticleTime >= 1.0f / 121.0f) {
            float dx,dy,dz,deltax,deltay,deltaz;
            float f;
            float rsquared;
            float mag;

            dx = s->old[0] - sx;
            dy = s->old[1] - sy;
            dz = s->old[2] - sz;
            mag = 5.0f;
            deltax = (dx * mag);
            deltay = (dy * mag);
            deltaz = (dz * mag);
            for(i=0;i<flurry->numStreams;i++) {
                float streamSpeedCoherenceFactor;
                
                s->p[s->nextParticle].delta[0].f[s->nextSubParticle] = deltax;
                s->p[s->nextParticle].delta[1].f[s->nextSubParticle] = deltay;
                s->p[s->nextParticle].delta[2].f[s->nextSubParticle] = deltaz;
                s->p[s->nextParticle].position[0].f[s->nextSubParticle] = sx;
                s->p[s->nextParticle].position[1].f[s->nextSubParticle] = sy;
                s->p[s->nextParticle].position[2].f[s->nextSubParticle] = sz;
                s->p[s->nextParticle].oldposition[0].f[s->nextSubParticle] = sx;
                s->p[s->nextParticle].oldposition[1].f[s->nextSubParticle] = sy;
                s->p[s->nextParticle].oldposition[2].f[s->nextSubParticle] = sz;
                streamSpeedCoherenceFactor = MAX_(0.0f,1.0f + RandBell(0.25f*incohesion));
                dx = s->p[s->nextParticle].position[0].f[s->nextSubParticle] - flurry->spark[i]->position[0];
                dy = s->p[s->nextParticle].position[1].f[s->nextSubParticle] - flurry->spark[i]->position[1];
                dz = s->p[s->nextParticle].position[2].f[s->nextSubParticle] - flurry->spark[i]->position[2];
                rsquared = (dx*dx+dy*dy+dz*dz);
                f = streamSpeed * streamSpeedCoherenceFactor;

                mag = f / (float) sqrt(rsquared);
                /*
                    reciprocal square-root estimate replaced above divide and call to system sqrt()
                
                    asm("frsqrte %0, %1" : "=f" (mag) : "f" (rsquared));
                    mag *= f;
                */

                s->p[s->nextParticle].delta[0].f[s->nextSubParticle] -= (dx * mag);
                s->p[s->nextParticle].delta[1].f[s->nextSubParticle] -= (dy * mag);
                s->p[s->nextParticle].delta[2].f[s->nextSubParticle] -= (dz * mag);
                s->p[s->nextParticle].color[0].f[s->nextSubParticle] = flurry->spark[i]->color[0] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[1].f[s->nextSubParticle] = flurry->spark[i]->color[1] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[2].f[s->nextSubParticle] = flurry->spark[i]->color[2] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[3].f[s->nextSubParticle] = 0.85f * (1.0f + RandBell(0.5f*colorIncoherence));
                s->p[s->nextParticle].time.f[s->nextSubParticle] = flurry->fTime;
                s->p[s->nextParticle].dead.i[s->nextSubParticle] = 0;
                s->p[s->nextParticle].animFrame.i[s->nextSubParticle] = random()&63;
                s->nextSubParticle++;
                if (s->nextSubParticle==4) {
                    s->nextParticle++;
                    s->nextSubParticle=0;
                }
                if (s->nextParticle >= NUMSMOKEPARTICLES/4) {
                    s->nextParticle = 0;
                    s->nextSubParticle = 0;
                }
            }

            s->lastParticleTime = flurry->fTime;
        }
    } else {
        s->lastParticleTime = flurry->fTime;
        s->firstTime = 0;
    }

    for(i=0;i<3;i++) {
        s->old[i] = flurry->star->position[i];
    }
    
    frameRate = ((double) flurry->dframe)/(flurry->fTime);
    frameRateModifier = 42.5f / frameRate;

    for(i=0;i<NUMSMOKEPARTICLES/4;i++) {        
        for(k=0; k<4; k++) {
            float dx,dy,dz;
            float f;
            float rsquared;
            float mag;
            float deltax;
            float deltay;
            float deltaz;
        
            if (s->p[i].dead.i[k]) {
                continue;
            }
            
            deltax = s->p[i].delta[0].f[k];
            deltay = s->p[i].delta[1].f[k];
            deltaz = s->p[i].delta[2].f[k];
            
            for(j=0;j<flurry->numStreams;j++) {
                dx = s->p[i].position[0].f[k] - flurry->spark[j]->position[0];
                dy = s->p[i].position[1].f[k] - flurry->spark[j]->position[1];
                dz = s->p[i].position[2].f[k] - flurry->spark[j]->position[2];
                rsquared = (dx*dx+dy*dy+dz*dz);

                /*
                    asm("fres %0, %1" : "=f" (f) : "f" (rsquared)); 
                    f *= gravity*frameRateModifier;
                */
                f = ( gravity  * frameRateModifier ) / rsquared;
                
                if((((i*4)+k) % flurry->numStreams) == j) {
                    f *= 1.0f + streamBias;
                }

                mag = f / (float) sqrt(rsquared); 

                /* reciprocal square-root estimate replaced above divide and call to system sqrt() */
                
                deltax -= (dx * mag);
                deltay -= (dy * mag);
                deltaz -= (dz * mag);
            }
    
            /* slow this particle down by flurry->drag */
            deltax *= flurry->drag;
            deltay *= flurry->drag;
            deltaz *= flurry->drag;
            
            if((deltax*deltax+deltay*deltay+deltaz*deltaz) >= 25000000.0f) {
                s->p[i].dead.i[k] = 1;
                continue;
            }
    
            /* update the position */
            s->p[i].delta[0].f[k] = deltax;
            s->p[i].delta[1].f[k] = deltay;
            s->p[i].delta[2].f[k] = deltaz;
            for(j=0;j<3;j++) {
                s->p[i].oldposition[j].f[k] = s->p[i].position[j].f[k];
                s->p[i].position[j].f[k] += (s->p[i].delta[j].f[k])*flurry->fDeltaTime;
            }
        }
    }
}

#endif

#ifdef __VEC__

void UpdateSmoke_VectorBase(global_info_t *global, flurry_info_t *flurry, SmokeV *s)
{
    unsigned int i,j;
    float sx = flurry->star->position[0];
    float sy = flurry->star->position[1];
    float sz = flurry->star->position[2];
    double frameRate;
    floatToVector frameRateModifier;
    floatToVector gravityV;
    floatToVector dragV;
    floatToVector deltaTimeV;
    const vector float deadConst = (vector float) (25000000.0,25000000.0,25000000.0,25000000.0);
    const vector float zero = (vector float)(0.0, 0.0, 0.0, 0.0);
    const vector float biasConst = (vector float)(streamBias);
    
    gravityV.f[0] = gravity;
    gravityV.v = (vector float) vec_splat((vector unsigned int)gravityV.v, 0);

    dragV.f[0] = flurry->drag;
    dragV.v = (vector float) vec_splat((vector unsigned int)dragV.v, 0);

    deltaTimeV.f[0] = flurry->fDeltaTime;
    deltaTimeV.v = (vector float) vec_splat((vector unsigned int)deltaTimeV.v, 0);

    s->frame++;

    if(!s->firstTime) {
        /* release 12 puffs every frame */
        if(flurry->fTime - s->lastParticleTime >= 1.0f / 121.0f) {
            float dx,dy,dz,deltax,deltay,deltaz;
            float f;
            float rsquared;
            float mag;

            dx = s->old[0] - sx;
            dy = s->old[1] - sy;
            dz = s->old[2] - sz;
            mag = 5.0f;
            deltax = (dx * mag);
            deltay = (dy * mag);
            deltaz = (dz * mag);
            for(i=0;i<flurry->numStreams;i++) {
                float streamSpeedCoherenceFactor;
                
                s->p[s->nextParticle].delta[0].f[s->nextSubParticle] = deltax;
                s->p[s->nextParticle].delta[1].f[s->nextSubParticle] = deltay;
                s->p[s->nextParticle].delta[2].f[s->nextSubParticle] = deltaz;
                s->p[s->nextParticle].position[0].f[s->nextSubParticle] = sx;
                s->p[s->nextParticle].position[1].f[s->nextSubParticle] = sy;
                s->p[s->nextParticle].position[2].f[s->nextSubParticle] = sz;
                s->p[s->nextParticle].oldposition[0].f[s->nextSubParticle] = sx;
                s->p[s->nextParticle].oldposition[1].f[s->nextSubParticle] = sy;
                s->p[s->nextParticle].oldposition[2].f[s->nextSubParticle] = sz;
                streamSpeedCoherenceFactor = MAX_(0.0f,1.0f + RandBell(0.25f*incohesion));
                dx = s->p[s->nextParticle].position[0].f[s->nextSubParticle] - flurry->spark[i]->position[0];
                dy = s->p[s->nextParticle].position[1].f[s->nextSubParticle] - flurry->spark[i]->position[1];
                dz = s->p[s->nextParticle].position[2].f[s->nextSubParticle] - flurry->spark[i]->position[2];
                rsquared = (dx*dx+dy*dy+dz*dz);
                f = streamSpeed * streamSpeedCoherenceFactor;

                mag = f / (float) sqrt(rsquared);
                /*
                    asm("frsqrte %0, %1" : "=f" (mag) : "f" (rsquared));
                    mag *= f;
                */

                s->p[s->nextParticle].delta[0].f[s->nextSubParticle] -= (dx * mag);
                s->p[s->nextParticle].delta[1].f[s->nextSubParticle] -= (dy * mag);
                s->p[s->nextParticle].delta[2].f[s->nextSubParticle] -= (dz * mag);
                s->p[s->nextParticle].color[0].f[s->nextSubParticle] = flurry->spark[i]->color[0] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[1].f[s->nextSubParticle] = flurry->spark[i]->color[1] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[2].f[s->nextSubParticle] = flurry->spark[i]->color[2] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[3].f[s->nextSubParticle] = 0.85f * (1.0f + RandBell(0.5f*colorIncoherence));
                s->p[s->nextParticle].time.f[s->nextSubParticle] = flurry->fTime;
                s->p[s->nextParticle].dead.i[s->nextSubParticle] = 0;
                s->p[s->nextParticle].animFrame.i[s->nextSubParticle] = random()&63;
                s->nextSubParticle++;
                if (s->nextSubParticle==4) {
                    s->nextParticle++;
                    s->nextSubParticle=0;
                }
                if (s->nextParticle >= NUMSMOKEPARTICLES/4) {
                    s->nextParticle = 0;
                    s->nextSubParticle = 0;
                }
            }

            s->lastParticleTime = flurry->fTime;
        }
    } else {
        s->lastParticleTime = flurry->fTime;
        s->firstTime = 0;
    }

    for(i=0;i<3;i++) {
        s->old[i] = flurry->star->position[i];
    }
    
    frameRate = ((double) flurry->dframe)/(flurry->fTime);
    frameRateModifier.f[0] = 42.5f / frameRate;
    frameRateModifier.v = (vector float) vec_splat((vector unsigned int)frameRateModifier.v, 0);
    
    frameRateModifier.v = vec_madd(frameRateModifier.v, gravityV.v, zero);
    
    for(i=0;i<NUMSMOKEPARTICLES/4;i++) {		
        /* floatToVector f; */
        vector float deltax, deltay, deltaz;
        vector float distTemp;
        vector unsigned int deadTemp;
        /* floatToVector infopos0, infopos1, infopos2; */
        intToVector mod;
        vector unsigned int jVec;
    
    
        vec_dst((int *)(&(s->p[i+4])), 0x00020200, 3);

        if (vec_all_ne(s->p[i].dead.v, (vector unsigned int)(0))) {
            continue;
        }
        
        deltax = s->p[i].delta[0].v;
        deltay = s->p[i].delta[1].v;
        deltaz = s->p[i].delta[2].v;
        
        mod.i[0] = (i<<2 + 0) % flurry->numStreams;
        if(mod.i[0]+1 == flurry->numStreams) {
            mod.i[1] = 0;
        } else {
            mod.i[1] = mod.i[0]+1;
        }
        if(mod.i[1]+1 == flurry->numStreams) {
            mod.i[2] = 0;
        } else {
            mod.i[2] = mod.i[1]+1;
        }
        if(mod.i[2]+1 == flurry->numStreams) {
            mod.i[3] = 0;
        } else {
            mod.i[3] = mod.i[2]+1;
        }
        
        jVec = vec_xor(jVec, jVec);
        
        vec_dst( &flurry->spark[0]->position[0], 0x16020160, 3 );       
        for(j=0; j<flurry->numStreams;j++) {
            vector float ip0, ip1 = (vector float)(0.0), ip2;
            vector float dx, dy, dz;
            vector float rsquared, f;
            vector float one_over_rsquared;
            vector float biasTemp;
            vector float mag;
            vector bool int biasOr;
            
            ip0 = vec_ld(0, flurry->spark[j]->position);
            if(((int)(flurry->spark[j]->position) & 0xF)>=8) {
                ip1 = vec_ld(16, flurry->spark[j]->position);
            }

            ip0 = vec_perm(ip0, ip1, vec_lvsl(0, flurry->spark[j]->position));
            ip1 = (vector float) vec_splat((vector unsigned int)ip0, 1);
            ip2 = (vector float) vec_splat((vector unsigned int)ip0, 2);
            ip0 = (vector float) vec_splat((vector unsigned int)ip0, 0);

            dx = vec_sub(s->p[i].position[0].v, ip0);
            dy = vec_sub(s->p[i].position[1].v, ip1);
            dz = vec_sub(s->p[i].position[2].v, ip2);
            
            rsquared = vec_madd(dx, dx, zero);
            rsquared = vec_madd(dy, dy, rsquared);
            rsquared = vec_madd(dz, dz, rsquared);
            
            biasOr = vec_cmpeq(jVec, mod.v);
            biasTemp = vec_add(vec_and(biasOr, biasConst), (vector float)(1.0));

            f = vec_madd(biasTemp, frameRateModifier.v, zero);
            one_over_rsquared = vec_re(rsquared);
            f = vec_madd(f, one_over_rsquared, zero);

            mag = vec_rsqrte(rsquared);
            mag = vec_madd(mag, f, zero);
            
            deltax = vec_nmsub(dx, mag, deltax);
            deltay = vec_nmsub(dy, mag, deltay);
            deltaz = vec_nmsub(dz, mag, deltaz);
           
            jVec = vec_add(jVec, (vector unsigned int)(1));
        }

        /* slow this particle down by flurry->drag */
        deltax = vec_madd(deltax, dragV.v, zero);
        deltay = vec_madd(deltay, dragV.v, zero);
        deltaz = vec_madd(deltaz, dragV.v, zero);
        
        distTemp = vec_madd(deltax, deltax, zero);
        distTemp = vec_madd(deltay, deltay, distTemp);
        distTemp = vec_madd(deltaz, deltaz, distTemp);
        
        deadTemp = (vector unsigned int) vec_cmpge(distTemp, deadConst);
        deadTemp = vec_and((vector unsigned int)vec_splat_u32(1), deadTemp);
        s->p[i].dead.v = vec_or(s->p[i].dead.v, deadTemp);
        if (vec_all_ne(s->p[i].dead.v, (vector unsigned int)(0))) {
            continue;
        }
        
        /* update the position */
        s->p[i].delta[0].v = deltax;
        s->p[i].delta[1].v = deltay;
        s->p[i].delta[2].v = deltaz;
        for(j=0;j<3;j++) {
            s->p[i].oldposition[j].v = s->p[i].position[j].v;
            s->p[i].position[j].v = vec_madd(s->p[i].delta[j].v, deltaTimeV.v, s->p[i].position[j].v);
        }
    }
}

void UpdateSmoke_VectorUnrolled(global_info_t *info, SmokeV *s)
{
    unsigned int i,j;
    float sx = flurry->star->position[0];
    float sy = flurry->star->position[1];
    float sz = flurry->star->position[2];
    double frameRate;
    floatToVector frameRateModifier;
    floatToVector gravityV;
    floatToVector dragV;
    floatToVector deltaTimeV;
    const vector float deadConst = (vector float) (25000000.0,25000000.0,25000000.0,25000000.0);
    const vector float zero = (vector float)(0.0, 0.0, 0.0, 0.0);
    const vector float biasConst = (vector float)(streamBias);
    
    gravityV.f[0] = gravity;
    gravityV.v = (vector float) vec_splat((vector unsigned int)gravityV.v, 0);

    dragV.f[0] = flurry->drag;
    dragV.v = (vector float) vec_splat((vector unsigned int)dragV.v, 0);

    deltaTimeV.f[0] = flurry->fDeltaTime;
    deltaTimeV.v = (vector float) vec_splat((vector unsigned int)deltaTimeV.v, 0);

    s->frame++;

    if(!s->firstTime) {
        /* release 12 puffs every frame */
        if(flurry->fTime - s->lastParticleTime >= 1.0f / 121.0f) {
            float dx,dy,dz,deltax,deltay,deltaz;
            float f;
            float rsquared;
            float mag;

            dx = s->old[0] - sx;
            dy = s->old[1] - sy;
            dz = s->old[2] - sz;
            mag = 5.0f;
            deltax = (dx * mag);
            deltay = (dy * mag);
            deltaz = (dz * mag);
            for(i=0;i<flurry->numStreams;i++) {
                float streamSpeedCoherenceFactor;
                
                s->p[s->nextParticle].delta[0].f[s->nextSubParticle] = deltax;
                s->p[s->nextParticle].delta[1].f[s->nextSubParticle] = deltay;
                s->p[s->nextParticle].delta[2].f[s->nextSubParticle] = deltaz;
                s->p[s->nextParticle].position[0].f[s->nextSubParticle] = sx;
                s->p[s->nextParticle].position[1].f[s->nextSubParticle] = sy;
                s->p[s->nextParticle].position[2].f[s->nextSubParticle] = sz;
                s->p[s->nextParticle].oldposition[0].f[s->nextSubParticle] = sx;
                s->p[s->nextParticle].oldposition[1].f[s->nextSubParticle] = sy;
                s->p[s->nextParticle].oldposition[2].f[s->nextSubParticle] = sz;
                streamSpeedCoherenceFactor = MAX_(0.0f,1.0f + RandBell(0.25f*incohesion));
                dx = s->p[s->nextParticle].position[0].f[s->nextSubParticle] - flurry->spark[i]->position[0];
                dy = s->p[s->nextParticle].position[1].f[s->nextSubParticle] - flurry->spark[i]->position[1];
                dz = s->p[s->nextParticle].position[2].f[s->nextSubParticle] - flurry->spark[i]->position[2];
                rsquared = (dx*dx+dy*dy+dz*dz);
                f = streamSpeed * streamSpeedCoherenceFactor;

                mag = f / (float) sqrt(rsquared);
                /*
                    asm("frsqrte %0, %1" : "=f" (mag) : "f" (rsquared));
                    mag *= f;
                */

                s->p[s->nextParticle].delta[0].f[s->nextSubParticle] -= (dx * mag);
                s->p[s->nextParticle].delta[1].f[s->nextSubParticle] -= (dy * mag);
                s->p[s->nextParticle].delta[2].f[s->nextSubParticle] -= (dz * mag);
                s->p[s->nextParticle].color[0].f[s->nextSubParticle] = flurry->spark[i]->color[0] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[1].f[s->nextSubParticle] = flurry->spark[i]->color[1] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[2].f[s->nextSubParticle] = flurry->spark[i]->color[2] * (1.0f + RandBell(colorIncoherence));
                s->p[s->nextParticle].color[3].f[s->nextSubParticle] = 0.85f * (1.0f + RandBell(0.5f*colorIncoherence));
                s->p[s->nextParticle].time.f[s->nextSubParticle] = flurry->fTime;
                s->p[s->nextParticle].dead.i[s->nextSubParticle] = 0;
                s->p[s->nextParticle].animFrame.i[s->nextSubParticle] = random()&63;
                s->nextSubParticle++;
                if (s->nextSubParticle==4) {
                    s->nextParticle++;
                    s->nextSubParticle=0;
                }
                if (s->nextParticle >= NUMSMOKEPARTICLES/4) {
                    s->nextParticle = 0;
                    s->nextSubParticle = 0;
                }
            }

            s->lastParticleTime = flurry->fTime;
        }
    } else {
        s->lastParticleTime = flurry->fTime;
        s->firstTime = 0;
    }

    for(i=0;i<3;i++) {
        s->old[i] = flurry->star->position[i];
    }
    
    frameRate = ((double) flurry->dframe)/(flurry->fTime);
    frameRateModifier.f[0] = 42.5f / frameRate;
    frameRateModifier.v = (vector float) vec_splat((vector unsigned int)frameRateModifier.v, 0);
    
    frameRateModifier.v = vec_madd(frameRateModifier.v, gravityV.v, zero);
    
    for(i=0;i<NUMSMOKEPARTICLES/4;i++) {		
        /* floatToVector f; */
        vector float deltax, deltay, deltaz;
        vector float distTemp;
        vector unsigned int deadTemp;
        /* floatToVector infopos0, infopos1, infopos2; */
        intToVector mod;
        vector unsigned int jVec;
        vector unsigned int intOne = vec_splat_u32(1);
        vector float floatOne = vec_ctf(intOne, 0);
    
    
        vec_dst((int *)(&(s->p[i+4])), 0x00020200, 3);

        if (vec_all_ne(s->p[i].dead.v, (vector unsigned int)(0))) {
            continue;
        }
        
        deltax = s->p[i].delta[0].v;
        deltay = s->p[i].delta[1].v;
        deltaz = s->p[i].delta[2].v;
        
        mod.i[0] = (i<<2 + 0) % flurry->numStreams;
        if(mod.i[0]+1 == flurry->numStreams) {
            mod.i[1] = 0;
        } else {
            mod.i[1] = mod.i[0]+1;
        }
        if(mod.i[1]+1 == flurry->numStreams) {
            mod.i[2] = 0;
        } else {
            mod.i[2] = mod.i[1]+1;
        }
        if(mod.i[2]+1 == flurry->numStreams) {
            mod.i[3] = 0;
        } else {
            mod.i[3] = mod.i[2]+1;
        }
        
        jVec = vec_xor(jVec, jVec);
        
        vec_dst( &flurry->spark[0]->position[0], 0x16020160, 3 );
        for(j=0; j + 3 < flurry->numStreams;j+=4) 
        {
            vector float dxa, dya, dza;
            vector float dxb, dyb, dzb;
            vector float dxc, dyc, dzc;
            vector float dxd, dyd, dzd;
            vector float ip0a, ip1a;
            vector float ip0b, ip1b;
            vector float ip0c, ip1c;
            vector float ip0d, ip1d;
            vector float rsquaredA;
            vector float rsquaredB;
            vector float rsquaredC;
            vector float rsquaredD;
            vector float fA, fB, fC, fD;
            vector float biasTempA;
            vector float biasTempB;
            vector float biasTempC;
            vector float biasTempD;
            vector float magA;
            vector float magB;
            vector float magC;
            vector float magD;

            vector float one_over_rsquaredA;
            vector float one_over_rsquaredB;
            vector float one_over_rsquaredC;
            vector float one_over_rsquaredD;
            vector bool int biasOrA,biasOrB,biasOrC,biasOrD;
            
            /* load vectors */
            ip0a = vec_ld(0, flurry->spark[j]->position);
            ip0b = vec_ld(0, flurry->spark[j+1]->position);
            ip0c = vec_ld(0, flurry->spark[j+2]->position);
            ip0d = vec_ld(0, flurry->spark[j+3]->position);
            ip1a = vec_ld( 12, flurry->spark[j]->position );
            ip1b = vec_ld( 12, flurry->spark[j+1]->position );
            ip1c = vec_ld( 12, flurry->spark[j+2]->position );
            ip1d = vec_ld( 12, flurry->spark[j+3]->position );
            
            /* align them */
            ip0a = vec_perm(ip0a, ip1a, vec_lvsl(0, flurry->spark[j]->position));
            ip0b = vec_perm(ip0b, ip1b, vec_lvsl(0, flurry->spark[j+1]->position));
            ip0c = vec_perm(ip0c, ip1c, vec_lvsl(0, flurry->spark[j+2]->position));
            ip0d = vec_perm(ip0d, ip1d, vec_lvsl(0, flurry->spark[j+3]->position));

            dxa = vec_splat( ip0a, 0  );
            dxb = vec_splat( ip0b, 0  );
            dxc = vec_splat( ip0c, 0  );
            dxd = vec_splat( ip0d, 0  );
            dxa = vec_sub( s->p[i].position[0].v, dxa );    
            dxb = vec_sub( s->p[i].position[0].v, dxb );    
            dxc = vec_sub( s->p[i].position[0].v, dxc );    
            dxd = vec_sub( s->p[i].position[0].v, dxd );    

            dya = vec_splat( ip0a, 1  );
            dyb = vec_splat( ip0b, 1  );
            dyc = vec_splat( ip0c, 1  );
            dyd = vec_splat( ip0d, 1  );
            dya = vec_sub( s->p[i].position[1].v, dya );    
            dyb = vec_sub( s->p[i].position[1].v, dyb );    
            dyc = vec_sub( s->p[i].position[1].v, dyc );    
            dyd = vec_sub( s->p[i].position[1].v, dyd );    

            dza = vec_splat( ip0a, 2  );
            dzb = vec_splat( ip0b, 2  );
            dzc = vec_splat( ip0c, 2  );
            dzd = vec_splat( ip0d, 2  );
            dza = vec_sub( s->p[i].position[2].v, dza );    
            dzb = vec_sub( s->p[i].position[2].v, dzb );    
            dzc = vec_sub( s->p[i].position[2].v, dzc );    
            dzd = vec_sub( s->p[i].position[2].v, dzd );    
            
            rsquaredA = vec_madd( dxa, dxa, zero );
            rsquaredB = vec_madd( dxb, dxb, zero );
            rsquaredC = vec_madd( dxc, dxc, zero );
            rsquaredD = vec_madd( dxd, dxd, zero );

            rsquaredA = vec_madd( dya, dya, rsquaredA );
            rsquaredB = vec_madd( dyb, dyb, rsquaredB );
            rsquaredC = vec_madd( dyc, dyc, rsquaredC );
            rsquaredD = vec_madd( dyd, dyd, rsquaredD );

            rsquaredA = vec_madd( dza, dza, rsquaredA );
            rsquaredB = vec_madd( dzb, dzb, rsquaredB );
            rsquaredC = vec_madd( dzc, dzc, rsquaredC );
            rsquaredD = vec_madd( dzd, dzd, rsquaredD );
        
            biasOrA = vec_cmpeq( jVec, mod.v );
            jVec = vec_add(jVec, intOne);
            biasOrB = vec_cmpeq( jVec, mod.v );
            jVec = vec_add(jVec, intOne);
            biasOrC = vec_cmpeq( jVec, mod.v );
            jVec = vec_add(jVec, intOne);
            biasOrD = vec_cmpeq( jVec, mod.v );
            jVec = vec_add(jVec, intOne);

            biasTempA = vec_add( vec_and( biasOrA, biasConst), floatOne);
            biasTempB = vec_add( vec_and( biasOrB, biasConst), floatOne);
            biasTempC = vec_add( vec_and( biasOrC, biasConst), floatOne);
            biasTempD = vec_add( vec_and( biasOrD, biasConst), floatOne);
            
            fA = vec_madd( biasTempA, frameRateModifier.v, zero);
            fB = vec_madd( biasTempB, frameRateModifier.v, zero);
            fC = vec_madd( biasTempC, frameRateModifier.v, zero);
            fD = vec_madd( biasTempD, frameRateModifier.v, zero);
            one_over_rsquaredA = vec_re( rsquaredA );
            one_over_rsquaredB = vec_re( rsquaredB );
            one_over_rsquaredC = vec_re( rsquaredC );
            one_over_rsquaredD = vec_re( rsquaredD );
            fA = vec_madd( fA, one_over_rsquaredA, zero);
            fB = vec_madd( fB, one_over_rsquaredB, zero);
            fC = vec_madd( fC, one_over_rsquaredC, zero);
            fD = vec_madd( fD, one_over_rsquaredD, zero);
            magA = vec_rsqrte( rsquaredA );
            magB = vec_rsqrte( rsquaredB );
            magC = vec_rsqrte( rsquaredC );
            magD = vec_rsqrte( rsquaredD );
            magA = vec_madd( magA, fA, zero );
            magB = vec_madd( magB, fB, zero );
            magC = vec_madd( magC, fC, zero );
            magD = vec_madd( magD, fD, zero );
            deltax = vec_nmsub( dxa, magA, deltax );
            deltay = vec_nmsub( dya, magA, deltay );
            deltaz = vec_nmsub( dza, magA, deltaz );
            
            deltax = vec_nmsub( dxb, magB, deltax );
            deltay = vec_nmsub( dyb, magB, deltay );
            deltaz = vec_nmsub( dzb, magB, deltaz );
            
            deltax = vec_nmsub( dxc, magC, deltax );
            deltay = vec_nmsub( dyc, magC, deltay );
            deltaz = vec_nmsub( dzc, magC, deltaz );
            
            deltax = vec_nmsub( dxd, magD, deltax );
            deltay = vec_nmsub( dyd, magD, deltay );
            deltaz = vec_nmsub( dzd, magD, deltaz );
        }

       
        for(;j<flurry->numStreams;j++) {
            vector float ip0, ip1 = (vector float)(0.0), ip2;
            vector float dx, dy, dz;
            vector float rsquared, f;
            vector float one_over_rsquared;
            vector float biasTemp;
            vector float mag;
            vector bool int biasOr;
            
            ip0 = vec_ld(0, flurry->spark[j]->position);
            if(((int)(flurry->spark[j]->position) & 0xF)>=8) {
                ip1 = vec_ld(16, flurry->spark[j]->position);
            }

            ip0 = vec_perm(ip0, ip1, vec_lvsl(0, flurry->spark[j]->position));
            ip1 = (vector float) vec_splat((vector unsigned int)ip0, 1);
            ip2 = (vector float) vec_splat((vector unsigned int)ip0, 2);
            ip0 = (vector float) vec_splat((vector unsigned int)ip0, 0);

            dx = vec_sub(s->p[i].position[0].v, ip0);
            dy = vec_sub(s->p[i].position[1].v, ip1);
            dz = vec_sub(s->p[i].position[2].v, ip2);
            
            rsquared = vec_madd(dx, dx, zero);
            rsquared = vec_madd(dy, dy, rsquared);
            rsquared = vec_madd(dz, dz, rsquared);
            
            biasOr = vec_cmpeq(jVec, mod.v);
            biasTemp = vec_add(vec_and(biasOr, biasConst), (vector float)(1.0));

            f = vec_madd(biasTemp, frameRateModifier.v, zero);
            one_over_rsquared = vec_re(rsquared);
            f = vec_madd(f, one_over_rsquared, zero);

            mag = vec_rsqrte(rsquared);
            mag = vec_madd(mag, f, zero);
            
            deltax = vec_nmsub(dx, mag, deltax);
            deltay = vec_nmsub(dy, mag, deltay);
            deltaz = vec_nmsub(dz, mag, deltaz);
           
            jVec = vec_add(jVec, (vector unsigned int)(1));
        }

        /* slow this particle down by flurry->drag */
        deltax = vec_madd(deltax, dragV.v, zero);
        deltay = vec_madd(deltay, dragV.v, zero);
        deltaz = vec_madd(deltaz, dragV.v, zero);
        
        distTemp = vec_madd(deltax, deltax, zero);
        distTemp = vec_madd(deltay, deltay, distTemp);
        distTemp = vec_madd(deltaz, deltaz, distTemp);
        
        deadTemp = (vector unsigned int) vec_cmpge(distTemp, deadConst);
        deadTemp = vec_and((vector unsigned int)vec_splat_u32(1), deadTemp);
        s->p[i].dead.v = vec_or(s->p[i].dead.v, deadTemp);
        if (vec_all_ne(s->p[i].dead.v, (vector unsigned int)(0))) {
            continue;
        }
        
        /* update the position */
        s->p[i].delta[0].v = deltax;
        s->p[i].delta[1].v = deltay;
        s->p[i].delta[2].v = deltaz;
        for(j=0;j<3;j++) {
            s->p[i].oldposition[j].v = s->p[i].position[j].v;
            s->p[i].position[j].v = vec_madd(s->p[i].delta[j].v, deltaTimeV.v, s->p[i].position[j].v);
        }
    }
}

#endif
#endif /* 0 */

void DrawSmoke_Scalar(global_info_t *global, flurry_info_t *flurry, SmokeV *s, float brightness)
{
	int svi = 0;
	int sci = 0;
	int sti = 0;
	int si = 0;
	float width;
        float sx,sy;
	float u0,v0,u1,v1;
	float w,z;
	float screenRatio = global->sys_glWidth / 1024.0f;
	float hslash2 = global->sys_glHeight * 0.5f;
	float wslash2 = global->sys_glWidth * 0.5f;
	int i,k;

	width = (streamSize+2.5f*flurry->streamExpansion) * screenRatio;

	for (i=0;i<NUMSMOKEPARTICLES/4;i++)
	{
            for (k=0; k<4; k++) {
		float thisWidth;
                float oldz;
                
                if (s->p[i].dead.i[k]) {
                    continue;
		}
		thisWidth = (streamSize + (flurry->fTime - s->p[i].time.f[k])*flurry->streamExpansion) * screenRatio;
		if (thisWidth >= width)
		{
			s->p[i].dead.i[k] = 1;
			continue;
		}
		z = s->p[i].position[2].f[k];
		sx = s->p[i].position[0].f[k] * global->sys_glWidth / z + wslash2;
		sy = s->p[i].position[1].f[k] * global->sys_glWidth / z + hslash2;
		oldz = s->p[i].oldposition[2].f[k];
		if (sx > global->sys_glWidth+50.0f || sx < -50.0f || sy > global->sys_glHeight+50.0f || sy < -50.0f || z < 25.0f || oldz < 25.0f)
		{
			continue;
		}

		w = MAX_(1.0f,thisWidth/z);
		{
			float oldx = s->p[i].oldposition[0].f[k];
			float oldy = s->p[i].oldposition[1].f[k];
			float oldscreenx = (oldx * global->sys_glWidth / oldz) + wslash2;
			float oldscreeny = (oldy * global->sys_glWidth / oldz) + hslash2;
			float dx = (sx-oldscreenx);
			float dy = (sy-oldscreeny);
					
			float d = FastDistance2D(dx, dy);
			
			float sm, os, ow;
			if (d)
			{
				sm = w/d;
			}
			else
			{
				sm = 0.0f;
			}
			ow = MAX_(1.0f,thisWidth/oldz);
			if (d)
			{
				os = ow/d;
			}
			else
			{
				os = 0.0f;
			}
			
			{
				floatToVector cmv;
                                float cm;
				float m = 1.0f + sm; 
		
				float dxs = dx*sm;
				float dys = dy*sm;
				float dxos = dx*os;
				float dyos = dy*os;
				float dxm = dx*m;
				float dym = dy*m;
		
				s->p[i].animFrame.i[k]++;
				if (s->p[i].animFrame.i[k] >= 64)
				{
					s->p[i].animFrame.i[k] = 0;
				}
		
				u0 = (s->p[i].animFrame.i[k]& 7) * 0.125f;
				v0 = (s->p[i].animFrame.i[k]>>3) * 0.125f;
				u1 = u0 + 0.125f;
				v1 = v0 + 0.125f;
				cm = (1.375f - thisWidth/width);
				if (s->p[i].dead.i[k] == 3)
				{
					cm *= 0.125f;
					s->p[i].dead.i[k] = 1;
				}
				si++;
				cm *= brightness;
				cmv.f[0] = s->p[i].color[0].f[k]*cm;
				cmv.f[1] = s->p[i].color[1].f[k]*cm;
				cmv.f[2] = s->p[i].color[2].f[k]*cm;
				cmv.f[3] = s->p[i].color[3].f[k]*cm;

#if 0
                                /* MDT we can't use vectors in the Scalar routine */
                                s->seraphimColors[sci++].v = cmv.v;
                                s->seraphimColors[sci++].v = cmv.v;
                                s->seraphimColors[sci++].v = cmv.v;
                                s->seraphimColors[sci++].v = cmv.v;
#else
                                {
                                    int ii, jj;
                                    for (jj = 0; jj < 4; jj++) {
                                        for (ii = 0; ii < 4; ii++) {
                                            s->seraphimColors[sci].f[ii] = cmv.f[ii];
                                        }
                                        sci += 1;
                                    }
                                }
#endif
                                
                                s->seraphimTextures[sti++] = u0;
                                s->seraphimTextures[sti++] = v0;
                                s->seraphimTextures[sti++] = u0;
                                s->seraphimTextures[sti++] = v1;

                                s->seraphimTextures[sti++] = u1;
                                s->seraphimTextures[sti++] = v1;
                                s->seraphimTextures[sti++] = u1;
                                s->seraphimTextures[sti++] = v0;
                                
                                s->seraphimVertices[svi].f[0] = sx+dxm-dys;
                                s->seraphimVertices[svi].f[1] = sy+dym+dxs;
                                s->seraphimVertices[svi].f[2] = sx+dxm+dys;
                                s->seraphimVertices[svi].f[3] = sy+dym-dxs;
                                svi++;                            
                        
                                s->seraphimVertices[svi].f[0] = oldscreenx-dxm+dyos;
                                s->seraphimVertices[svi].f[1] = oldscreeny-dym-dxos;
                                s->seraphimVertices[svi].f[2] = oldscreenx-dxm-dyos;
                                s->seraphimVertices[svi].f[3] = oldscreeny-dym+dxos;
                                svi++;
			}
		}
            }
	}
	glColorPointer(4,GL_FLOAT,0,s->seraphimColors);
	glVertexPointer(2,GL_FLOAT,0,s->seraphimVertices);
	glTexCoordPointer(2,GL_FLOAT,0,s->seraphimTextures);
	glDrawArrays(GL_QUADS,0,si*4);
}

#if 0
#ifdef __VEC__

void DrawSmoke_Vector(global_info_t *global, flurry_info_t *flurry, SmokeV *s, float brightness)
{
    const vector float zero = (vector float)(0.0);
    int svi = 0;
    int sci = 0;
    int sti = 0;
    int si = 0;
    floatToVector width;
    vector float sx,sy;
    floatToVector u0,v0,u1,v1;
    vector float one_over_z;
    vector float w;
    floatToVector z;
    float screenRatio = global->sys_glWidth / 1024.0f;
    float hslash2 = global->sys_glHeight * 0.5f;
    float wslash2 = global->sys_glWidth * 0.5f;
    int i,kk;
    floatToVector briteV, fTimeV, expansionV, screenRatioV, hslash2V, wslash2V, streamSizeV;
    floatToVector glWidthV;
    floatToVector cm;
    vector float cmv[4];
    vector float svec[4], ovec[4];
    vector float oldscreenx, oldscreeny;
    vector float sm;
    vector float frameAnd7;
    vector float frameShift3;
    vector float one_over_width;
    vector float dx, dy;
    vector float os;
    vector unsigned int vSi = vec_splat_u32(0);
    const vector float eighth = (vector float)(0.125);
    float glWidth50 = global->sys_glWidth + 50.0f;
    float glHeight50 = global->sys_glHeight + 50.0f;
    vector float vGLWidth50, vGLHeight50;
    unsigned int blitBool;

    vec_dst((int *)(&(s->p[0])), 0x00020200, 2);
    
    {
        vector unsigned char permute1 = vec_lvsl( 0, &glWidth50 );
        vector unsigned char permute2 = vec_lvsl( 0, &glHeight50 );
        permute1 = (vector unsigned char) vec_splat( (vector unsigned int) permute1, 0 );
        permute2 = (vector unsigned char) vec_splat( (vector unsigned int) permute2, 0 );
        vGLWidth50 = vec_lde( 0, &glWidth50 );
        vGLHeight50 = vec_lde( 0, &glHeight50 );
        vGLWidth50 = vec_perm( vGLWidth50, vGLWidth50, permute1 );
        vGLHeight50 = vec_perm( vGLHeight50, vGLHeight50, permute2 );
    }

    width.f[0] = (streamSize+2.5f*flurry->streamExpansion) * screenRatio;
    width.v = (vector float) vec_splat((vector unsigned int)width.v, 0);

    briteV.f[0] = brightness;
    briteV.v = (vector float) vec_splat((vector unsigned int)briteV.v, 0);
    
    fTimeV.f[0] = (float) flurry->fTime;
    fTimeV.v = (vector float) vec_splat((vector unsigned int)fTimeV.v, 0);
    
    expansionV.f[0] = flurry->streamExpansion;
    expansionV.v = (vector float) vec_splat((vector unsigned int)expansionV.v, 0);
    
    screenRatioV.f[0] = screenRatio;
    screenRatioV.v = (vector float) vec_splat((vector unsigned int)screenRatioV.v, 0);
            
    hslash2V.f[0] = hslash2;
    hslash2V.v = (vector float) vec_splat((vector unsigned int)hslash2V.v, 0);

    wslash2V.f[0] = wslash2;
    wslash2V.v = (vector float) vec_splat((vector unsigned int)wslash2V.v, 0);

    streamSizeV.f[0] = streamSize;
    streamSizeV.v = (vector float) vec_splat((vector unsigned int)streamSizeV.v, 0);

    glWidthV.f[0] = global->sys_glWidth;
    glWidthV.v = (vector float) vec_splat((vector unsigned int)glWidthV.v, 0);

    for (i=0;i<NUMSMOKEPARTICLES/4;i++) {
        vector float thisWidth;
        vector float oldz;
        vector float oldx, oldy, one_over_oldz;
        vector float xabs, yabs, mn;                
        vector float d; 
        vector float one_over_d;
        vector bool int dnz;
        vector float ow;

        vec_dst((int *)(&(s->p[i+4])), 0x00020200, 2);

        if (vec_all_eq(s->p[i].dead.v, (vector unsigned int)(1))) continue;
            
        blitBool = 0; /* keep track of particles that actually need to be drawn */
        
        thisWidth = vec_sub(fTimeV.v, s->p[i].time.v);
        thisWidth = vec_madd(thisWidth, expansionV.v, streamSizeV.v);
        thisWidth = vec_madd(thisWidth, screenRatioV.v, zero);

        z.v = s->p[i].position[2].v;
        one_over_z = vec_re(z.v);

        sx = vec_madd(s->p[i].position[0].v, glWidthV.v, zero);
        sx = vec_madd(sx, one_over_z, wslash2V.v);
        sy = vec_madd(s->p[i].position[1].v, glWidthV.v, zero);
        sy = vec_madd(sy, one_over_z, hslash2V.v);

        oldz = s->p[i].oldposition[2].v;
        
        w = vec_max((vector float)(1.0), vec_madd(thisWidth, one_over_z, zero));

        oldx = s->p[i].oldposition[0].v;
        oldy = s->p[i].oldposition[1].v;
        one_over_oldz = vec_re(oldz);
        oldscreenx = vec_madd(oldx, glWidthV.v, zero);
        oldscreenx = vec_madd(oldscreenx, one_over_oldz, wslash2V.v);
        oldscreeny = vec_madd(oldy, glWidthV.v, zero);
        oldscreeny = vec_madd(oldscreeny, one_over_oldz, hslash2V.v);
        dx = vec_sub(sx,oldscreenx);
        dy = vec_sub(sy,oldscreeny);

        xabs = vec_abs(dx);
        yabs = vec_abs(dy);
        mn = vec_min(xabs,yabs);
        d = vec_add(xabs,yabs);
        d = vec_madd(mn, (vector float)(-0.6875), d);

        ow = vec_max((vector float)(1.0), vec_madd(thisWidth, one_over_oldz, zero));
        one_over_d = vec_re(d);
        dnz = vec_cmpgt(d, zero);
        sm = vec_madd(w, one_over_d, zero);
        sm = vec_and(sm, dnz);
        os = vec_madd(ow, one_over_d, zero);
        os = vec_and(os, dnz);
       
        {
            intToVector tempMask;
            vector bool int mask = vec_cmpeq( s->p[i].dead.v, vec_splat_u32(1) ); /* -1 where true */
            vector bool int  gtMask = vec_cmpge( thisWidth, width.v ); /* -1 where true */
            vector bool int  glWidth50Test = vec_cmpgt( sx, (vector float)(vGLWidth50) ); /* -1 where true */
            vector bool int  glHeight50Test = vec_cmpgt( sy, (vector float)(vGLHeight50) ); /* -1 where true */
            vector bool int  test50x 	= vec_cmplt( sx, (vector float) (-50.0) );
            vector bool int  test50y 	= vec_cmplt( sy, (vector float) (-50.0) );
            vector bool int  testz 	= vec_cmplt( z.v, (vector float) (25.0) );
            vector bool int  testoldz 	= vec_cmplt( oldz, (vector float) (25.0) );
            mask = vec_or( mask, gtMask );
            s->p[i].dead.v = vec_and( mask, vec_splat_u32( 1 ) );
            mask = vec_or( mask, glWidth50Test );
            mask = vec_or( mask, glHeight50Test );
            mask = vec_or( mask, test50x );
            mask = vec_or( mask, test50y );
            mask = vec_or( mask, testz );
            mask = vec_or( mask, testoldz );
            tempMask.v = (vector unsigned int)mask;
        
            s->p[i].animFrame.v = vec_sub( s->p[i].animFrame.v, vec_nor( mask, mask ) );
            s->p[i].animFrame.v = vec_and( s->p[i].animFrame.v, (vector unsigned int)(63) );
    
            frameAnd7 = vec_ctf(vec_and(s->p[i].animFrame.v, (vector unsigned int)(7)),0);
            u0.v = vec_madd(frameAnd7, eighth, zero);
            
            frameShift3 = vec_ctf(vec_sr(s->p[i].animFrame.v, (vector unsigned int)(3)),0);
            v0.v = vec_madd(frameAnd7, eighth, zero);
    
            u1.v = vec_add(u0.v, eighth);
            v1.v = vec_add(v0.v, eighth);
    
            one_over_width = vec_re(width.v);
            cm.v = vec_sel( vec_nmsub(thisWidth, one_over_width, (vector float)(1.375)), cm.v, mask );
            cm.v = vec_madd(cm.v, briteV.v, zero);
    
            vSi = vec_sub( vSi, vec_nor( mask, mask ) );
            {
                vector unsigned int blitMask = (vector unsigned int) (1, 2, 4, 8);
                vector unsigned int temp = (vector unsigned int)mask;
                temp = vec_andc( blitMask, temp  );
                temp = vec_add( temp, vec_sld( temp, temp, 8 ) );
                temp = vec_add( temp, vec_sld( temp, temp, 4 ) );
                vec_ste( temp, 0, &blitBool );
    
            }
        
            {
                vector float temp1, temp2, temp3, temp4;
                vector float result1a, result1b, result2a, result2b, result3a, result3b, result4a, result4b;
            
                temp1 = vec_mergeh( u0.v, u0.v );
                temp2 = vec_mergel( u0.v, u0.v );
                temp3 = vec_mergeh( v0.v, v1.v );
                temp4 = vec_mergel( v0.v, v1.v );
    
                result1a = vec_mergeh( temp1, temp3 );
                result1b = vec_mergel( temp1, temp3 );
                result2a = vec_mergeh( temp2, temp4 );
                result2b = vec_mergel( temp2, temp4 );
    
                temp1 = vec_mergeh( u1.v, u1.v );
                temp2 = vec_mergel( u1.v, u1.v );
                temp3 = vec_mergeh( v1.v, v0.v );
                temp4 = vec_mergel( v1.v, v0.v );
    
                result3a = vec_mergeh( temp1, temp3 );
                result3b = vec_mergel( temp1, temp3 );
                result4a = vec_mergeh( temp2, temp4 );
                result4b = vec_mergel( temp2, temp4 );
    
                if( blitBool & 1 )
                {
                    vec_st( result1a, 0, &s->seraphimTextures[sti] );
                    vec_st( result3a, 16, &s->seraphimTextures[sti]);
                    sti+= 8;
                }
                if( blitBool & 2 )
                {
                    vec_st( result1b, 0, &s->seraphimTextures[sti]);
                    vec_st( result3b, 16, &s->seraphimTextures[sti]);
                    sti+= 8;
                }
                if( blitBool & 4 )
                {
                    vec_st( result2a, 0, &s->seraphimTextures[sti]);
                    vec_st( result4a, 16, &s->seraphimTextures[sti]);
                    sti+= 8;
                }
                if( blitBool & 8 )
                {
                    vec_st( result2b, 0, &s->seraphimTextures[sti]);
                    vec_st( result4b, 16, &s->seraphimTextures[sti]);
                    sti+= 8;
                }
            }
        }
        
        cmv[0] = vec_madd(s->p[i].color[0].v, cm.v, zero);
        cmv[1] = vec_madd(s->p[i].color[1].v, cm.v, zero);
        cmv[2] = vec_madd(s->p[i].color[2].v, cm.v, zero);
        cmv[3] = vec_madd(s->p[i].color[3].v, cm.v, zero);
        {
            vector float vI0, vI1, vI2, vI3;
    
            vI0 = vec_mergeh ( cmv[0], cmv[2] );
            vI1 = vec_mergeh ( cmv[1], cmv[3] );
            vI2 = vec_mergel ( cmv[0], cmv[2] );
            vI3 = vec_mergel ( cmv[1], cmv[3] );
    
            cmv[0] = vec_mergeh ( vI0, vI1 );
            cmv[1] = vec_mergel ( vI0, vI1 );
            cmv[2] = vec_mergeh ( vI2, vI3 );
            cmv[3] = vec_mergel ( vI2, vI3 );
        }

        vec_dst( cmv, 0x0D0100D0, 1 );

        {
            vector float sxd, syd;
            vector float sxdm, sxdp, sydm, sydp;
            vector float oxd, oyd;
            vector float oxdm, oxdp, oydm, oydp; 
            vector float vI0, vI1, vI2, vI3;
            vector float dxs, dys;
            vector float dxos, dyos;
            vector float dxm, dym;
            vector float m;
            
            m = vec_add((vector float)(1.0), sm); 
            
            dxs = vec_madd(dx, sm, zero);
            dys = vec_madd(dy, sm, zero);
            dxos = vec_madd(dx, os, zero);
            dyos = vec_madd(dy, os, zero);
            dxm = vec_madd(dx, m, zero);
            dym = vec_madd(dy, m, zero);

            sxd = vec_add(sx, dxm);
            sxdm = vec_sub(sxd, dys);
            sxdp = vec_add(sxd, dys);
            
            syd = vec_add(sy, dym);
            sydm = vec_sub(syd, dxs);
            sydp = vec_add(syd, dxs);  
            
            oxd = vec_sub(oldscreenx, dxm);
            oxdm = vec_sub(oxd, dyos);
            oxdp = vec_add(oxd, dyos);
            
            oyd = vec_sub(oldscreeny, dym);
            oydm = vec_sub(oyd, dxos);
            oydp = vec_add(oyd, dxos);
            
            vI0 = vec_mergeh ( sxdm, sxdp );
            vI1 = vec_mergeh ( sydp, sydm );
            vI2 = vec_mergel ( sxdm, sxdp );
            vI3 = vec_mergel ( sydp, sydm );
    
            svec[0] = vec_mergeh ( vI0, vI1 );
            svec[1] = vec_mergel ( vI0, vI1 );
            svec[2] = vec_mergeh ( vI2, vI3 );
            svec[3] = vec_mergel ( vI2, vI3 );

            vI0 = vec_mergeh ( oxdp, oxdm );
            vI1 = vec_mergeh ( oydm, oydp );
            vI2 = vec_mergel ( oxdp, oxdm );
            vI3 = vec_mergel ( oydm, oydp );
    
            ovec[0] = vec_mergeh ( vI0, vI1 );
            ovec[1] = vec_mergel ( vI0, vI1 );
            ovec[2] = vec_mergeh ( vI2, vI3 );
            ovec[3] = vec_mergel ( vI2, vI3 );
        }

        {
            int offset0 = (sci + 0) * sizeof( vector float );
            int offset1 = (sci + 1) * sizeof( vector float );
            int offset2 = (sci + 2) * sizeof( vector float );
            int offset3 = (sci + 3) * sizeof( vector float );
            int offset4 = (svi + 0) * sizeof( vector float );
            int offset5 = (svi + 1) * sizeof( vector float );
            vector float *colors = (vector float *)s->seraphimColors;
            vector float *vertices = (vector float *)s->seraphimVertices;
            for (kk=0; kk<4; kk++) {
                if (blitBool>>kk & 1) {
                    vector float vcmv = cmv[kk];
                    vector float vsvec = svec[kk];
                    vector float vovec = ovec[kk];
    
                    vec_st( vcmv, offset0, colors );
                    vec_st( vcmv, offset1, colors );
                    vec_st( vcmv, offset2, colors );
                    vec_st( vcmv, offset3, colors );
                    vec_st( vsvec, offset4, vertices );
                    vec_st( vovec, offset5, vertices );
                    colors += 4;
                    vertices += 2;
                    sci += 4;
                    svi += 2;                
                }
            }
        }
    }
    vSi = vec_add( vSi, vec_sld( vSi, vSi, 8 ) );
    vSi = vec_add( vSi, vec_sld( vSi, vSi, 4 ) );
    vec_ste( (vector signed int) vSi, 0, &si );

    glColorPointer(4,GL_FLOAT,0,s->seraphimColors);
    glVertexPointer(2,GL_FLOAT,0,s->seraphimVertices);
    glTexCoordPointer(2,GL_FLOAT,0,s->seraphimTextures);
    glDrawArrays(GL_QUADS,0,si*4);
}

#endif
#endif /* 0 */
