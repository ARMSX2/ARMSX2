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


#ifndef SHADERS_H
#define SHADERS_H


#ifdef WIN32
#include <windows.h>
#include "extensions.h"
#endif
#include <GL/gl.h>
#include <string>


const std::string gooVertSource(
"varying vec4 vsPosition;\n"
"varying vec4 gl_TexCoord[1];\n"
"void main(void){\n"
"	vsPosition = gl_ModelViewMatrix * gl_Vertex;\n"
"	gl_FrontColor = gl_Color;\n"
"	gl_BackColor = gl_Color;\n"
"	gl_TexCoord[0] = vec4(gl_Normal, 1.0);\n"
"	gl_Position = ftransform();\n"
"}\n"
);

const std::string gooFragSource(
"varying vec4 vsPosition;\n"
"varying vec4 gl_TexCoord[1];\n"
"uniform samplerCube normaltex0;\n"
"uniform samplerCube normaltex1;\n"
"uniform samplerCube tex;\n"
"void main(void){\n"
"	vec4 normal0 = textureCube(normaltex0, gl_TexCoord[0].xyz) * 2.0 - 1.0;\n"
"	vec4 normal1 = textureCube(normaltex1, gl_TexCoord[0].xyz) * 2.0 - 1.0;\n"
"	vec4 texnormal = mix(normal0, normal1, gl_Color.a);\n"
"	// Reflection vector\n"
"	vec3 eyevec = normalize(vsPosition.xyz);\n"
"	float eyedotnorm = dot(eyevec, texnormal.xyz);\n"
"	vec3 reflectvec = (texnormal.xyz * (eyedotnorm * 2.0)) - eyevec;\n"
"	// Reflected color\n"
"	vec4 color = textureCube(tex, reflectvec);\n"
"	// Fresnel reflection\n"
"	float fresnelalpha = 1.0 - abs(eyedotnorm);\n"
"	color *= fresnelalpha * fresnelalpha * fresnelalpha;\n"
"	// Overbrighten and modulate with vertex color using a mix of regular multiplicative and subtractive alpha\n"
"	color.rgb = 2.0 * mix(color.rgb * gl_Color.rgb, max(vec3(0.0, 0.0, 0.0), color.rgb + gl_Color.rgb - vec3(1.0, 1.0, 1.0)), 0.6);\n"
"	// Depth cue\n"
"	color = mix(gl_Fog.color, color, clamp((gl_Fog.end + vsPosition.z) * gl_Fog.scale, 0.0, 1.0));\n" 
"	gl_FragColor = color;\n"
"}\n"
);


const std::string tunnelVertSource(
"varying vec4 vsPosition;\n"
"varying vec4 gl_TexCoord[1];\n"
"void main(void){\n"
"	vsPosition = gl_ModelViewMatrix * gl_Vertex;\n"
"	gl_FrontColor = gl_Color;\n"
"	gl_BackColor = gl_Color;\n"
"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
"	gl_Position = ftransform();\n"
"}\n"
);

const std::string tunnelFragSource(
"varying vec4 vsPosition;\n"
"varying vec4 gl_TexCoord[1];\n"
"uniform sampler2D tex0;\n"
"uniform sampler2D tex1;\n"
"void main(void){\n"
"	vec4 texcol0 = texture2D(tex0, gl_TexCoord[0].xy);\n"
"	texcol0 += 0.5 * texture2D(tex0, gl_TexCoord[0].xy * vec2(3.0, 2.0));\n"
"	vec4 texcol1 = texture2D(tex1, gl_TexCoord[0].xy);\n"
"	texcol1 += 0.5 * texture2D(tex1, gl_TexCoord[0].xy * vec2(3.0, 2.0));\n"
"	vec4 color = mix(texcol0, texcol1, gl_Color.a);\n"
"	// Note that max value of color at this point is 1.5\n"
"	// Overbrighten and modulate with vertex color using a mix of regular multiplicative and subtractive alpha\n"
"	color.rgb = 2.0 * mix(gl_Color.rgb * color.rgb, max(vec3(0.0, 0.0, 0.0), (gl_Color.rgb * vec3(1.5, 1.5, 1.5)) + color.rgb - vec3(1.5, 1.5, 1.5)), 0.5);\n"
"	// Depth cue\n"
"	color = mix(gl_Fog.color, color, clamp((gl_Fog.end + vsPosition.z) * gl_Fog.scale, 0.0, 1.0));\n" 
"	gl_FragColor = color;\n"
"}\n"
);


GLhandleARB gooProgram;
GLhandleARB tunnelProgram;


void initShaders(){
	GLhandleARB vertShader = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);
	const GLchar* gooVertString = gooVertSource.c_str();
	glShaderSourceARB(vertShader, 1, &gooVertString, NULL);
	glCompileShaderARB(vertShader);

	GLhandleARB fragShader = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
	const GLchar* gooFragString = gooFragSource.c_str();
	glShaderSourceARB(fragShader, 1, &gooFragString, NULL);
	glCompileShaderARB(fragShader);

	gooProgram = glCreateProgramObjectARB();
	glAttachObjectARB(gooProgram, vertShader);
	glAttachObjectARB(gooProgram, fragShader);
	glLinkProgramARB(gooProgram);

	glUseProgramObjectARB(gooProgram);
	GLint loc = glGetUniformLocationARB(gooProgram, "normaltex0");
	glUniform1iARB(loc, 0);
	loc = glGetUniformLocationARB(gooProgram, "normaltex1");
	glUniform1iARB(loc, 1);
	loc = glGetUniformLocationARB(gooProgram, "tex");
	glUniform1iARB(loc, 2);

	vertShader = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);
	const GLchar* tunnelVertString = tunnelVertSource.c_str();
	glShaderSourceARB(vertShader, 1, &tunnelVertString, NULL);
	glCompileShaderARB(vertShader);

	fragShader = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
	const GLchar* tunnelFragString = tunnelFragSource.c_str();
	glShaderSourceARB(fragShader, 1, &tunnelFragString, NULL);
	glCompileShaderARB(fragShader);

	tunnelProgram = glCreateProgramObjectARB();
	glAttachObjectARB(tunnelProgram, vertShader);
	glAttachObjectARB(tunnelProgram, fragShader);
	glLinkProgramARB(tunnelProgram);

	glUseProgramObjectARB(tunnelProgram);
	loc = glGetUniformLocationARB(tunnelProgram, "tex0");
	glUniform1iARB(loc, 0);
	loc = glGetUniformLocationARB(tunnelProgram, "tex1");
	glUniform1iARB(loc, 1);

	glUseProgramObjectARB(0);
}


#endif
