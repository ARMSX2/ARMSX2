/*
 * Copyright (C) 2001-2010  Terence M. Welsh
 *
 * This file is part of Implicit.
 *
 * Implicit is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Implicit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "impSurface.h"

#include <iostream>
#include <string.h>

#include <GL/glext.h>

#include <rsMath/rsMath.h>

#ifdef WIN32
PFNGLMULTIDRAWELEMENTSPROC impSurface::glMultiDrawElements = NULL;
PFNGLGENBUFFERSPROC impSurface::glGenBuffers = NULL;
PFNGLDELETEBUFFERSPROC impSurface::glDeleteBuffers = NULL;
PFNGLBINDBUFFERPROC impSurface::glBindBuffer = NULL;
PFNGLBUFFERDATAPROC impSurface::glBufferData = NULL;
#endif

impSurface::impSurface()
{
	mUseVBOs = true;
	mCompile = false;

#ifdef WIN32
	// try to initialize extensions
	if (glMultiDrawElements == NULL)
	{
		if (queryExtension("GL_ARB_vertex_buffer_object"))
		{
			glMultiDrawElements = (PFNGLMULTIDRAWELEMENTSPROC)getProcAddr("glMultiDrawElements");
			glGenBuffers = (PFNGLGENBUFFERSPROC)getProcAddr("glGenBuffers");
			glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)getProcAddr("glDeleteBuffers");
			glBindBuffer = (PFNGLBINDBUFFERPROC)getProcAddr("glBindBuffer");
			glBufferData = (PFNGLBUFFERDATAPROC)getProcAddr("glBufferData");
		}
	}

	// If any extensions were not found, fall back to draw arrays
	if (glMultiDrawElements == NULL || glGenBuffers == NULL || glDeleteBuffers == NULL
	    || glBindBuffer == NULL || glBufferData == NULL)
		mUseVBOs = false;
#endif

	index_offset = 0;
	vertex_offset = 0;
	num_tristrips = 0;
	triStripLengths.resize(0);
	vertices.resize(0);
	indices.resize(0);
	vertex_data_size = sizeof(float) * 6;

	if (mUseVBOs)
	{
		glGenBuffers(1, &vbo_array_id);
		glGenBuffers(1, &vbo_index_id);
	}
}

impSurface::~impSurface()
{
	triStripLengths.resize(0);
	indices.resize(0);
	vertices.resize(0);

	if (mUseVBOs)
	{
		glDeleteBuffers(1, &vbo_array_id);
		glDeleteBuffers(1, &vbo_index_id);
	}
}

#ifdef WIN32
int
impSurface::queryExtension(char* name)
{
	char* extensions = (char*)glGetString(GL_EXTENSIONS);
	char* start = extensions;
	char* position;
	char* end;
	int quit = 0;

	while (quit == 0)
	{
		position = strstr(start, name);
		if (!position)
			quit = 1;
		else
		{
			// is name a substring of a larger name?
			end = position + strlen(name);
			if (position == start || *(position - 1) == ' ')
			{
				if (*end == ' ' || *end == '\0')
					return 1;
			}
			start = end;
		}
	}

	return 0;
}

void*
impSurface::getProcAddr(char* name)
{
	void* addr = (void*)wglGetProcAddress(name);
	return addr;
	//void *addr = (void *)glXGetProcAddressARB((char *)name);
	//return addr;
}
#endif

void
impSurface::reset()
{
	num_tristrips = 0;
	index_offset = 0;
	vertex_offset = 0;

	// New data is going to be created, so VBO or display list must be compiled again.
	mCompile = true;
}

#ifdef USE_TRIANGLE_STRIPS
void
impSurface::addTriStripLength(unsigned char length)
{
	// make more tristrip storage if necessary
	const size_t tslsize(triStripLengths.size());
	if (num_tristrips == tslsize)
		triStripLengths.resize(tslsize + 1000);

	triStripLengths[num_tristrips++] = length;
}
#endif

void
impSurface::addIndex(unsigned int index)
{
	// make more vertex data storage if necessary
	const size_t datasize(indices.size());
	if (index_offset == datasize)
		indices.resize(datasize + 1000);

#if USE_UNSIGNED_SHORT
	indices[index_offset++] = static_cast<unsigned short>(index);
#else
	indices[index_offset++] = index;
#endif
}

void
impSurface::addVertex(float* data)
{
	// make more vertex data storage if necessary
	const size_t datasize(vertices.size());
	if ((vertex_offset + 6) >= datasize)
		vertices.resize(datasize + 1000);

	memcpy(&(vertices[vertex_offset]), data, 24);//vertex_data_size);
	vertex_offset += 6;
}

#define BUFFER_OFFSET(i) ((char *)NULL + (i))

void
impSurface::draw()
{
	if (index_offset == 0)
		return;

	// Draw using Vertex Buffer Objects
	if (mUseVBOs)
	{
#if USE_TRIANGLE_STRIPS
		if (mCompile)
		{
			if (vbo_index_offsets.size() < triStripLengths.size())
				vbo_index_offsets.resize(triStripLengths.size());
			unsigned int offset = 0;
#if USE_UNSIGNED_SHORT
			const unsigned int index_size(sizeof(GLushort));
#else
			const unsigned int index_size(sizeof(GLuint));
#endif
			for (unsigned int i = 0; i < triStripLengths.size(); ++i)
			{
				vbo_index_offsets[i] = (GLvoid*)(offset * index_size);
				//vbo_index_offsets[i] = (GLvoid*)(&(indices[offset]));
				offset += triStripLengths[i];
			}
		}
#endif

		// create a data store for vertex information and fill it with the vertices
		glBindBuffer(GL_ARRAY_BUFFER, vbo_array_id);
		glNormalPointer(GL_FLOAT, 6 * sizeof(float), (const GLvoid*)0);
		glVertexPointer(3, GL_FLOAT, 6 * sizeof(float), (const GLvoid*)(3 * sizeof(float)));
		if (mCompile)
			glBufferData(GL_ARRAY_BUFFER, vertex_offset * sizeof(GLfloat), &(vertices[0]), GL_DYNAMIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo_index_id);
		if (mCompile)
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_offset * sizeof(GLuint), &(indices[0]), GL_DYNAMIC_DRAW);

		glEnableClientState(GL_NORMAL_ARRAY);
		glEnableClientState(GL_VERTEX_ARRAY);
#if USE_TRIANGLE_STRIPS
#if USE_UNSIGNED_SHORT
		glMultiDrawElements(GL_TRIANGLE_STRIP, (const GLsizei*)(&(triStripLengths[0])),
		    GL_UNSIGNED_SHORT, (const GLvoid**)(&(vbo_index_offsets[0])), num_tristrips);
#else
		glMultiDrawElements(GL_TRIANGLE_STRIP, (const GLsizei*)(&(triStripLengths[0])),
		    GL_UNSIGNED_INT, (const GLvoid**)(&(vbo_index_offsets[0])), num_tristrips);
#endif
#else
#if USE_UNSIGNED_SHORT
		glDrawElements(GL_TRIANGLES, index_offset, GL_UNSIGNED_SHORT, BUFFER_OFFSET(0));
#else
		glDrawElements(GL_TRIANGLES, index_offset, GL_UNSIGNED_INT, BUFFER_OFFSET(0));
#endif
#endif
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_NORMAL_ARRAY);

		// Do not compile the same data again; draw this same data again.
		mCompile = false;
	}
	else
	{
		// Draw using Vertex Arrays
		glInterleavedArrays(GL_N3F_V3F, 0, &(vertices[0]));
#if USE_TRIANGLE_STRIPS
		int start_vert = 0;
		for (unsigned int i = 0; i < num_tristrips; ++i)
		{
#if USE_UNSIGNED_SHORT
			glDrawElements(GL_TRIANGLE_STRIP, triStripLengths[i], GL_UNSIGNED_SHORT, &(indices[start_vert]));
#else
			glDrawElements(GL_TRIANGLE_STRIP, triStripLengths[i], GL_UNSIGNED_INT, &(indices[start_vert]));
#endif
			start_vert += triStripLengths[i];
		}
#else
#if USE_UNSIGNED_SHORT
		glDrawElements(GL_TRIANGLES, index_offset, GL_UNSIGNED_SHORT, &(indices[0]));
#else
		glDrawElements(GL_TRIANGLES, index_offset, GL_UNSIGNED_INT, &(indices[0]));
#endif
#endif
	}
}
