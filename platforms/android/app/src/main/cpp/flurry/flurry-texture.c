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

/*
 *  Texture.c
 *  AppleFlurry
 *
 *  Created by calumr on Sat Jul 07 2001.
 *  Copyright (c) 2001 __CompanyName__. All rights reserved.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "flurry.h"

/* #include <GL/gl.h> */
/* #include <GL/glu.h> */

#include <stdlib.h>
#include <math.h>

static GLubyte smallTextureArray[32][32];
static GLubyte bigTextureArray[256][256][2];

/* simple smoothing routine */
static void SmoothTexture(void)
{
    GLubyte filter[32][32];
    int i,j;
    float t;
    for (i=1;i<31;i++)
    {
        for (j=1;j<31;j++)
        {
            t = (float) smallTextureArray[i][j]*4;
            t += (float) smallTextureArray[i-1][j];
            t += (float) smallTextureArray[i+1][j];
            t += (float) smallTextureArray[i][j-1];
            t += (float) smallTextureArray[i][j+1];
            t /= 8.0f;
            filter[i][j] = (GLubyte) t;
        }
    }
    for (i=1;i<31;i++)
    {
        for (j=1;j<31;j++)
        {
            smallTextureArray[i][j] = filter[i][j];
        }
    }
}

/* add some randomness to texture data */
static void SpeckleTexture(void)
{
    int i,j;
    int speck;
    float t;
    for (i=2;i<30;i++)
    {
        for (j=2;j<30;j++)
        {
            speck = 1;
            while (speck <= 32 && random() % 2)
            {
                t = (float) MIN_(255,smallTextureArray[i][j]+speck);
                smallTextureArray[i][j] = (GLubyte) t;
                speck+=speck;
            }
            speck = 1;
            while (speck <= 32 && random() % 2)
            {
                t = (float) MAX_(0,smallTextureArray[i][j]-speck);
                smallTextureArray[i][j] = (GLubyte) t;
                speck+=speck;
            }
        }
    }
}

static void MakeSmallTexture(void)
{
    static int firstTime = 1;
    int i,j;
    float r,t;
    if (firstTime)
    {
        firstTime = 0;
        for (i=0;i<32;i++)
        {
            for (j=0;j<32;j++)
            {
                r = (float) sqrt((i-15.5)*(i-15.5)+(j-15.5)*(j-15.5));
                if (r > 15.0f)
                {
                    smallTextureArray[i][j] = 0;
                }
                else
                {
                    t = 255.0f * (float) cos(r*M_PI/31.0);
                    smallTextureArray[i][j] = (GLubyte) t;
                }
            }
        }
    }
    else
    {
        for (i=0;i<32;i++)
        {
            for (j=0;j<32;j++)
            {
                r = (float) sqrt((i-15.5)*(i-15.5)+(j-15.5)*(j-15.5));
                if (r > 15.0f)
                {
                    t = 0.0f;
                }
                else
                {
                    t = 255.0f * (float) cos(r*M_PI/31.0);
                }
                smallTextureArray[i][j] = (GLubyte) MIN_(255,(t+smallTextureArray[i][j]+smallTextureArray[i][j])/3);
            }
        }
    }
    SpeckleTexture();
    SmoothTexture();
    SmoothTexture();
}

static void CopySmallTextureToBigTexture(int k, int l)
{
    int i,j;
    for (i=0;i<32;i++)
    {
        for (j=0;j<32;j++)
        {
            bigTextureArray[i+k][j+l][0] = smallTextureArray[i][j];
            bigTextureArray[i+k][j+l][1] = smallTextureArray[i][j];
        }
    }
}

static void AverageLastAndFirstTextures(void)
{
    int i,j;
    int t;
    for (i=0;i<32;i++)
    {
        for (j=0;j<32;j++)
        {
            t = (smallTextureArray[i][j] + bigTextureArray[i][j][0]) / 2;
            smallTextureArray[i][j] = (GLubyte) MIN_(255,t);
        }
    }
}

GLuint MakeTexture(void)
{
    GLuint theTexture = 0;
    int i,j;
    for (i=0;i<8;i++)
    {
        for (j=0;j<8;j++)
        {
            if (i==7 && j==7)
            {
                AverageLastAndFirstTextures();
            }
            else
            {
                MakeSmallTexture();
            }
            CopySmallTextureToBigTexture(i*32,j*32);
        }
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT,1);

    glGenTextures(1, &theTexture);
    glBindTexture(GL_TEXTURE_2D, theTexture);

    /* Set the tiling mode (this is generally always GL_REPEAT). */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    /* Set the filtering. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* GL_LINEAR, not LINEAR_MIPMAP_NEAREST.
     *
     * A mipmapped min filter makes the texture INCOMPLETE unless the whole chain exists, and an
     * incomplete texture samples as opaque black -- which, through this shader's c *= texture,
     * multiplies the entire particle to nothing. glGenerateMipmap is also not guaranteed for
     * GL_LUMINANCE_ALPHA, which is filterable but not colour-renderable, so the chain may never
     * have been built. The sprite is a 256x256 blob drawn at roughly its own size; the mip chain
     * was never doing much for it. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    /* gluBuild2DMipmaps upstream. There is no GLU on Android, and GLES2 builds the chain
     * itself -- the internal format has to be spelled out rather than given as a component
     * count, which is what the 2 meant in GL 1.x. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 256, 256, 0,
                 GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, bigTextureArray);
    /* No glGenerateMipmap: see the min filter above. */
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    return theTexture;
}
