/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenJK; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// ESSL 300 shader sources and the program cache for rd-gles3.
//
// G1 covers the 2D/UI subset: solid color and single-texture stages.  The
// multi-texture/blend/fog type variants fold onto the same generic programs
// (per-vertex colors and GL blend state carry the semantics); exact
// per-type shading arrives with G2/G4 together with the converted
// shaders/glsl templates.

#include "tr_local.h"

typedef struct gles3_prog_s {
	GLuint		program;
	uint32_t	def_state_bits;
	int			def_shader_type;
	int			def_fog_stage;
	int			def_acff;
} gles3_prog_t;

static gles3_prog_t g_progs[256];
static int g_progs_count;

static const char *const GLSL_VERSION = "#version 300 es\n";

static const char *const VS_GENERIC =
	"precision highp float;\n"
	"in vec3 in_xyz;\n"
	"in vec4 in_color;\n"
	"in vec2 in_st0;\n"
	"in vec2 in_st1;\n"
	"in vec2 in_st2;\n"
	"in vec3 in_normal;\n"
	"uniform mat4 u_MVP;\n"
	"uniform mat4 u_ModelMatrix;\n"
	"out vec4 v_color;\n"
	"out vec2 v_tc0;\n"
	"out vec2 v_tc1;\n"
	"out vec2 v_tc2;\n"
	"void main() {\n"
	"	gl_Position = u_MVP * vec4(in_xyz, 1.0);\n"
	"	v_color = in_color;\n"
	"	v_tc0 = in_st0;\n"
	"	v_tc1 = in_st1;\n"
	"	v_tc2 = in_st2;\n"
	"}\n";

static const char *const FS_COLOR =
	"precision mediump float;\n"
	"in vec4 v_color;\n"
	"out vec4 out_color;\n"
	"#ifdef ALPHA_TEST\n"
	"uniform float u_AlphaTest;\n"
	"#endif\n"
	"void main() {\n"
	"	vec4 c = v_color;\n"
	"#ifdef ALPHA_TEST\n"
	"	if (c.a < u_AlphaTest) discard;\n"
	"#endif\n"
	"	out_color = c;\n"
	"}\n";

static const char *const FS_TEXTURE =
	"precision mediump float;\n"
	"uniform sampler2D u_Texture0;\n"
	"in vec4 v_color;\n"
	"in vec2 v_tc0;\n"
	"out vec4 out_color;\n"
	"#ifdef ALPHA_TEST\n"
	"uniform float u_AlphaTest;\n"
	"#endif\n"
	"void main() {\n"
	"	vec4 c = texture(u_Texture0, v_tc0) * v_color;\n"
	"#ifdef ALPHA_TEST\n"
	"	if (c.a < u_AlphaTest) discard;\n"
	"#endif\n"
	"	out_color = c;\n"
	"}\n";

static const char *const FS_TEXTURE_FIXED_COLOR =
	"precision mediump float;\n"
	"uniform sampler2D u_Texture0;\n"
	"uniform vec4 u_FixedColor;\n"
	"in vec4 v_color;\n"
	"in vec2 v_tc0;\n"
	"out vec4 out_color;\n"
	"void main() {\n"
	"	out_color = texture(u_Texture0, v_tc0) * u_FixedColor;\n"
	"}\n";

static const char *const FS_FOG =
	"precision mediump float;\n"
	"uniform vec4 u_FogColor;\n"
	"in vec4 v_color;\n"
	"out vec4 out_color;\n"
	"void main() {\n"
	"	out_color = vec4(u_FogColor.rgb, u_FogColor.a * v_color.a);\n"
	"}\n";

static GLuint gles3_compile_shader( GLenum type, const char *src, const char *defines )
{
	char buf[4096];
	const GLchar *src_ptr;
	GLuint shader;
	GLint status;

	Com_sprintf( buf, sizeof(buf), "%s%s%s", GLSL_VERSION, defines ? defines : "", src );

	shader = glCreateShader( type );
	src_ptr = buf;
	glShaderSource( shader, 1, &src_ptr, NULL );
	glCompileShader( shader );
	glGetShaderiv( shader, GL_COMPILE_STATUS, &status );
	if ( status == 0 ) {
		char log[2048];
		glGetShaderInfoLog( shader, sizeof(log), NULL, log );
		ri.Error( ERR_FATAL, "gles3: shader compile failed:\n%s\n%s", buf, log );
	}
	return shader;
}

static GLuint gles3_link_program( const char *vs_defines, const char *fs_defines, const char *fs_src )
{
	GLuint vs, fs, program;
	GLint status;

	vs = gles3_compile_shader( GL_VERTEX_SHADER, VS_GENERIC, vs_defines );
	fs = gles3_compile_shader( GL_FRAGMENT_SHADER, fs_src, fs_defines );

	program = glCreateProgram();
	glAttachShader( program, vs );
	glAttachShader( program, fs );
	glLinkProgram( program );
	glGetProgramiv( program, GL_LINK_STATUS, &status );
	if ( status == 0 ) {
		char log[2048];
		glGetProgramInfoLog( program, sizeof(log), NULL, log );
		ri.Error( ERR_FATAL, "gles3: program link failed: %s", log );
	}

	glDeleteShader( vs );
	glDeleteShader( fs );
	return program;
}

void gles3_init_programs( void )
{
	g_progs_count = 0;
}

void gles3_destroy_programs( void )
{
	int i;
	for ( i = 0; i < g_progs_count; i++ ) {
		glDeleteProgram( g_progs[i].program );
	}
	g_progs_count = 0;
}

// Returns a cached program for the shader class of def.  The cache key covers
// the fragment variant and the alpha-test define; everything else (blend
// funcs, depth, cull) is dynamic GL state applied by vk_bind_pipeline.
GLuint gles3_get_program( const Vk_Pipeline_Def *def )
{
	const char *fs_src;
	const char *fs_defines = "";
	const char *vs_defines = "";
	uint32_t atest = def->state_bits & GLS_ATEST_BITS;
	int key = (int)def->shader_type * 16;
	int i;

	if ( atest )
		fs_defines = "#define ALPHA_TEST\n";

	if ( def->shader_type == TYPE_COLOR_BLACK || def->shader_type == TYPE_COLOR_WHITE ||
		 def->shader_type == TYPE_COLOR_GREEN || def->shader_type == TYPE_COLOR_RED ||
		 def->shader_type == TYPE_DOT || def->shader_type == TYPE_SINGLE_TEXTURE_DF ) {
		fs_src = FS_COLOR;
		if ( def->shader_type == TYPE_COLOR_BLACK )
			key = 1;
		else if ( def->shader_type == TYPE_COLOR_WHITE )
			key = 2;
		else if ( def->shader_type == TYPE_COLOR_GREEN )
			key = 3;
		else if ( def->shader_type == TYPE_COLOR_RED )
			key = 4;
		else
			key = 5;
	}
	else if ( def->shader_type == TYPE_FOG_ONLY ) {
		fs_src = FS_FOG;
		key = 6;
	}
	else if ( def->shader_type == TYPE_SINGLE_TEXTURE_FIXED_COLOR ) {
		fs_src = FS_TEXTURE_FIXED_COLOR;
		key = 7;
	}
	else {
		// TYPE_SINGLE_TEXTURE_* / *_ENV / MULTI_* / BLEND* - generic texture program
		fs_src = FS_TEXTURE;
		key = 8;
	}

	(void)vs_defines;

	for ( i = 0; i < g_progs_count; i++ ) {
		if ( g_progs[i].def_shader_type == key )
			return g_progs[i].program;
	}

	if ( g_progs_count >= (int)ARRAY_LEN( g_progs ) ) {
		ri.Error( ERR_FATAL, "gles3: program cache exhausted" );
	}

	g_progs[g_progs_count].program = gles3_link_program( vs_defines, fs_defines, fs_src );
	g_progs[g_progs_count].def_shader_type = key;
	g_progs[g_progs_count].def_state_bits = def->state_bits;
	g_progs_count++;

	return g_progs[g_progs_count - 1].program;
}
