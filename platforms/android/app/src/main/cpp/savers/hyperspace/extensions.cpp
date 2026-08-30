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


#include "extensions.h"



PFNGLACTIVETEXTUREARBPROC glActiveTextureARB;
PFNGLCREATESHADEROBJECTARBPROC glCreateShaderObjectARB;
PFNGLSHADERSOURCEARBPROC glShaderSourceARB;
PFNGLCOMPILESHADERARBPROC glCompileShaderARB;
PFNGLCREATEPROGRAMOBJECTARBPROC glCreateProgramObjectARB;
PFNGLATTACHOBJECTARBPROC glAttachObjectARB;
PFNGLLINKPROGRAMARBPROC glLinkProgramARB;
PFNGLUSEPROGRAMOBJECTARBPROC glUseProgramObjectARB;
PFNGLGETUNIFORMLOCATIONARBPROC glGetUniformLocationARB;
PFNGLUNIFORM1IARBPROC glUniform1iARB;



int queryExtension(char* name){
	char* extensions = (char *)glGetString(GL_EXTENSIONS);
	char* start = extensions;
	char* position;
	char* end;
	int quit = 0;

	while(quit == 0){
		position = strstr(start, name);
		if(!position)
			quit = 1;
		else{
			// is name a substring of a larger name?
			end = position + strlen(name);
			if(position == start || *(position-1) == ' '){
				if(*end == ' ' || *end == '\0')
					return 1;
			}
			start = end;
		}
	}

	return 0;
}


void* getProcAddr(char* name){
#ifdef WIN32
	void *addr = (void *)wglGetProcAddress(name);
	return addr;
#else
	void *addr = (void *)glXGetProcAddressARB((char *)name);
	return addr;
#endif
}


int initExtensions(){
	if(queryExtension("GL_ARB_multitexture") && queryExtension("GL_ARB_texture_cube_map") && queryExtension("GL_ARB_shader_objects")){
		glActiveTextureARB = (PFNGLACTIVETEXTUREARBPROC)getProcAddr("glActiveTextureARB");
		glCreateShaderObjectARB = (PFNGLCREATESHADEROBJECTARBPROC)getProcAddr("glCreateShaderObjectARB");
		glShaderSourceARB = (PFNGLSHADERSOURCEARBPROC)getProcAddr("glShaderSourceARB");
		glCompileShaderARB = (PFNGLCOMPILESHADERARBPROC)getProcAddr("glCompileShaderARB");
		glCreateProgramObjectARB = (PFNGLCREATEPROGRAMOBJECTARBPROC)getProcAddr("glCreateProgramObjectARB");
		glAttachObjectARB = (PFNGLATTACHOBJECTARBPROC)getProcAddr("glAttachObjectARB");
		glLinkProgramARB = (PFNGLLINKPROGRAMARBPROC)getProcAddr("glLinkProgramARB");
		glUseProgramObjectARB = (PFNGLUSEPROGRAMOBJECTARBPROC)getProcAddr("glUseProgramObjectARB");
		glGetUniformLocationARB = (PFNGLGETUNIFORMLOCATIONARBPROC)getProcAddr("glGetUniformLocationARB");
		glUniform1iARB = (PFNGLUNIFORM1IARBPROC)getProcAddr("glUniform1iARB");
		return 1;
	}

	return 0;
}
