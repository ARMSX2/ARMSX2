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

#ifndef IMPSURFACE_H
#define IMPSURFACE_H

#ifdef WIN32
#include <windows.h>
#endif
#include <vector>
#include <cstddef>

#include <GL/gl.h>
#include <GL/glext.h>

#define USE_UNSIGNED_SHORT 0  // use short instead of int when passing indices to draw calls
#define USE_TRIANGLE_STRIPS 0  // use triangle strips instead of triangles

class impSurface
{
private:
	unsigned int index_offset;
	unsigned int vertex_offset;
	unsigned int num_tristrips;
	std::vector<unsigned int> triStripLengths;
	std::vector<float> vertices;
	size_t vertex_data_size;
#if USE_UNSIGNED_SHORT
	std::vector<unsigned short> indices;
#else
	std::vector<unsigned int> indices;
#endif

	// display list
	GLuint mDisplayList;

	// vbo stuff
	bool mUseVBOs;  // Default to true.  If extensions aren't found, set to false and use draw arrays.
	bool mCompile;  // If there is new data, set this to true to compile new VBO buffers.
	GLuint vbo_array_id;
	GLuint vbo_index_id;
	std::vector<GLvoid*> vbo_index_offsets;
#ifdef WIN32
	// extensions necessary for VBOs
	static PFNGLMULTIDRAWELEMENTSPROC glMultiDrawElements;
	static PFNGLGENBUFFERSPROC glGenBuffers;
	static PFNGLDELETEBUFFERSPROC glDeleteBuffers;
	static PFNGLBINDBUFFERPROC glBindBuffer;
	static PFNGLBUFFERDATAPROC glBufferData;
#endif

public:
	impSurface();
	~impSurface();

#ifdef WIN32
	int queryExtension(char* name);
	void* getProcAddr(char* name);
#endif

	// Set data counts to 0
	void reset();

	// Add data to surface
#ifdef USE_TRIANGLE_STRIPS
	void addTriStripLength(unsigned char length);
#endif
	void addIndex(unsigned int index);
	void addVertex(float* data);  // provide array of 6 floats (normal, position)

	void draw();
	//void draw_wireframe();
};

#endif
