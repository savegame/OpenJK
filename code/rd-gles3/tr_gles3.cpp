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
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// rd-gles3 (AuroraOS): GLES3 shader/pipeline framework, GL state cache and
// geometry streaming. Modelled after the gles3 renderer in quake3e
// (code/renderergles3/gles3.c), adapted to C++ and to the JA rd-vanilla
// backend conventions (standard GL depth: LEQUAL, clear depth 1.0).
//
// Geometry streaming follows gles3_map_geometry_buffer/gles3_flush_geometry:
// one large VBO, orphaned once per frame, host shadow staged in plain malloc
// memory (NOT ri.Malloc - the SP zone is small and shared, see plan section 6
// risk 6) and uploaded right before each draw via glMapBufferRange with
// GL_MAP_UNSYNCHRONIZED_BIT.

#include "../server/exe_headers.h"

#include "tr_local.h"
#include "tr_gles3.h"

#include <stdlib.h>
#include <string.h>

void myGlMultMatrix( const float *a, const float *b, float *out );

#define G3_MAX_PROGRAMS		512
#define G3_VERTEX_BUFFER_SIZE	( 4 * 1024 * 1024 )

typedef struct {
	g3_pipeline_def_t	def;
	GLuint			program;
	GLint			loc_u_mvp;
	float			cached_mvp[16];
} g3_program_t;

typedef struct {
	GLuint		program;
	qboolean	blend;
	GLenum		srcBlend, dstBlend;
	qboolean	depthTest;
	qboolean	depthWrite;
	GLenum		depthFunc;
	qboolean	cull;
	GLenum		cullFace;
	GLenum		frontFace;
	qboolean	polygonOffset;
	float		offsetFactor, offsetUnits;
	float		depthRangeNear, depthRangeFar;
} g3_glstate_t;

static struct {
	g3_program_t	programs[G3_MAX_PROGRAMS];
	int		program_count;

	GLuint		geometry_buffer;
	GLuint		vao;
	qboolean	buffer_mapped;		// host cursor valid for the current frame

	// Host-side shadow of geometry_buffer. Plain malloc, NOT ri.Malloc:
	// ri.Malloc goes to the SP zone, which is shared with client/server
	// state; a multi-MB per-frame allocation there silently corrupts
	// unrelated subsystems (same precedent as gles3-quake3e gles3.c).
	byte		*geometry_host;
	uint32_t	geometry_host_size;
	uint32_t	geometry_size;		// current GL buffer size
	uint32_t	geometry_offset;	// write cursor
	uint32_t	geometry_uploaded;	// bytes already uploaded this frame

	g3_glstate_t	gl;

	uint32_t	attr_enabled;
	uint32_t	attr_pending;
} g3;

//
// GLSL source generation (GLSL ES 3.00, quake3e gen_vert/gen_frag as model)
//

static const char *g3_vertex_template =
	"precision highp float;\n"
	"layout(location = 0) in vec4 in_position;\n"
	"layout(location = 1) in vec4 in_color;\n"
	"#if TEXTURED\n"
	"layout(location = 2) in vec2 in_texcoord0;\n"
	"#endif\n"
	"uniform mat4 u_mvp;\n"
	"out vec4 v_color;\n"
	"#if TEXTURED\n"
	"out vec2 v_texcoord0;\n"
	"#endif\n"
	"void main() {\n"
	"	gl_Position = u_mvp * in_position;\n"
	"	v_color = in_color;\n"
	"#if TEXTURED\n"
	"	v_texcoord0 = in_texcoord0;\n"
	"#endif\n"
	"}\n";

static const char *g3_fragment_template =
	"precision mediump float;\n"
	"in vec4 v_color;\n"
	"#if TEXTURED\n"
	"in vec2 v_texcoord0;\n"
	"uniform sampler2D u_texture0;\n"
	"#endif\n"
	"out vec4 out_color;\n"
	"void main() {\n"
	"#if TEXTURED\n"
	"	vec4 c = texture(u_texture0, v_texcoord0) * v_color;\n"
	"#else\n"
	"	vec4 c = v_color;\n"
	"#endif\n"
	"	// alpha test: 1 = GT_0 (keep a > 0), 2 = LT_80 (keep a < 0.5), 3 = GE_80 (keep a >= 0.5)\n"
	"#if ATEST_FUNC == 1\n"
	"	if (c.a <= ATEST_VALUE) discard;\n"
	"#elif ATEST_FUNC == 2\n"
	"	if (c.a >= ATEST_VALUE) discard;\n"
	"#elif ATEST_FUNC == 3\n"
	"	if (c.a < ATEST_VALUE) discard;\n"
	"#endif\n"
	"	out_color = c;\n"
	"}\n";

static int g3_atest_func_for_bits( uint32_t state_bits )
{
	switch ( state_bits & GLS_ATEST_BITS ) {
		case GLS_ATEST_GT_0:	return 1;
		case GLS_ATEST_LT_80:	return 2;
		case GLS_ATEST_GE_80:	return 3;
		case GLS_ATEST_GE_C0:	return 3; // TODO(M3): GE_C0 wants 0.75, separate variant
		default:		return 0;
	}
}

static void g3_get_shader_sources( const g3_pipeline_def_t *def,
		char *vs, size_t vs_size, char *fs, size_t fs_size )
{
	int atest_func = g3_atest_func_for_bits( def->state_bits );
	float atest_value = ( atest_func == 1 ) ? 0.0f : 0.5f;
	int textured = ( def->type == G3_PROG_TEXTURE ) ? 1 : 0;

	Com_sprintf( vs, vs_size,
		"#version 300 es\n"
		"#define TEXTURED %d\n"
		"#define ATEST_FUNC 0\n"
		"#define ATEST_VALUE 0.0\n"
		"%s",
		textured, g3_vertex_template );

	Com_sprintf( fs, fs_size,
		"#version 300 es\n"
		"#define TEXTURED %d\n"
		"#define ATEST_FUNC %d\n"
		"#define ATEST_VALUE %.1f\n"
		"%s",
		textured, atest_func, atest_value, g3_fragment_template );
}

static GLuint g3_compile_shader( GLenum type, const char *name, const char *src )
{
	GLuint sh = glCreateShader( type );
	GLint status;
	glShaderSource( sh, 1, &src, NULL );
	glCompileShader( sh );
	glGetShaderiv( sh, GL_COMPILE_STATUS, &status );
	if ( !status ) {
		char log[4096];
		glGetShaderInfoLog( sh, sizeof( log ), NULL, log );
		ri.Printf( PRINT_ALL, "GLES3: %s shader compile failed:\n%s\n--- source ---\n%s\n",
			name, log, src );
		glDeleteShader( sh );
		return 0;
	}
	return sh;
}

static GLuint g3_create_program( const g3_pipeline_def_t *def )
{
	char vs_src[8192], fs_src[8192];
	GLuint vs, fs, prog;
	GLint status;

	g3_get_shader_sources( def, vs_src, sizeof( vs_src ), fs_src, sizeof( fs_src ) );

	vs = g3_compile_shader( GL_VERTEX_SHADER, "gles3_vertex", vs_src );
	if ( !vs )
		return 0;
	fs = g3_compile_shader( GL_FRAGMENT_SHADER, "gles3_fragment", fs_src );
	if ( !fs ) {
		glDeleteShader( vs );
		return 0;
	}

	prog = glCreateProgram();
	glAttachShader( prog, vs );
	glAttachShader( prog, fs );
	glLinkProgram( prog );
	glDeleteShader( vs );
	glDeleteShader( fs );

	glGetProgramiv( prog, GL_LINK_STATUS, &status );
	if ( !status ) {
		char log[4096];
		glGetProgramInfoLog( prog, sizeof( log ), NULL, log );
		ri.Printf( PRINT_ALL, "GLES3: program link failed:\n%s\n", log );
		glDeleteProgram( prog );
		return 0;
	}

	// sampler uniforms are program state in GL: set them once at creation
	glUseProgram( prog );
	glUniform1i( glGetUniformLocation( prog, "u_texture0" ), 0 );
	glUseProgram( 0 );

	return prog;
}

GLuint g3_get_program( const g3_pipeline_def_t *def )
{
	int i;
	g3_program_t *prog;

	for ( i = 0; i < g3.program_count; i++ ) {
		if ( memcmp( &g3.programs[i].def, def, sizeof( *def ) ) == 0 )
			return g3.programs[i].program;
	}

	if ( g3.program_count >= G3_MAX_PROGRAMS ) {
		ri.Printf( PRINT_WARNING, "GLES3: G3_MAX_PROGRAMS reached\n" );
		return 0;
	}

	prog = &g3.programs[g3.program_count];
	memset( prog, 0, sizeof( *prog ) );
	prog->def = *def;
	prog->program = g3_create_program( def );
	if ( prog->program ) {
		prog->loc_u_mvp = glGetUniformLocation( prog->program, "u_mvp" );
		memset( prog->cached_mvp, 0xff, sizeof( prog->cached_mvp ) ); // force upload
	}
	g3.program_count++;

	// creation used glUseProgram; make sure the next bind re-issues it
	g3.gl.program = 0;

	return prog->program;
}

//
// GL state cache (gles3.c state cache + gles3_apply_pipeline_state as model)
//

static GLenum g3_blend_factor( uint32_t bits )
{
	switch ( bits ) {
		case GLS_SRCBLEND_ZERO:			return GL_ZERO;
		case GLS_SRCBLEND_ONE:			return GL_ONE;
		case GLS_SRCBLEND_DST_COLOR:		return GL_DST_COLOR;
		case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:	return GL_ONE_MINUS_DST_COLOR;
		case GLS_SRCBLEND_SRC_ALPHA:		return GL_SRC_ALPHA;
		case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	return GL_ONE_MINUS_SRC_ALPHA;
		case GLS_SRCBLEND_DST_ALPHA:		return GL_DST_ALPHA;
		case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:	return GL_ONE_MINUS_DST_ALPHA;
		case GLS_SRCBLEND_ALPHA_SATURATE:	return GL_SRC_ALPHA_SATURATE;
		case GLS_DSTBLEND_ZERO:			return GL_ZERO;
		case GLS_DSTBLEND_ONE:			return GL_ONE;
		case GLS_DSTBLEND_SRC_COLOR:		return GL_SRC_COLOR;
		case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:	return GL_ONE_MINUS_SRC_COLOR;
		case GLS_DSTBLEND_SRC_ALPHA:		return GL_SRC_ALPHA;
		case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	return GL_ONE_MINUS_SRC_ALPHA;
		case GLS_DSTBLEND_DST_ALPHA:		return GL_DST_ALPHA;
		case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:	return GL_ONE_MINUS_DST_ALPHA;
		default:				return GL_ONE;
	}
}

static void g3_apply_blend_depth( uint32_t state_bits )
{
	g3_glstate_t *gl = &g3.gl;

	// blend
	{
		uint32_t src_bits = state_bits & GLS_SRCBLEND_BITS;
		uint32_t dst_bits = state_bits & GLS_DSTBLEND_BITS;
		qboolean blend = ( src_bits || dst_bits ) ? qtrue : qfalse;
		GLenum src = blend ? g3_blend_factor( src_bits ) : GL_ONE;
		GLenum dst = blend ? g3_blend_factor( dst_bits ) : GL_ZERO;

		if ( gl->blend != blend || ( blend && ( gl->srcBlend != src || gl->dstBlend != dst ) ) ) {
			if ( blend ) {
				if ( !gl->blend )
					glEnable( GL_BLEND );
				glBlendFunc( src, dst );
				gl->srcBlend = src;
				gl->dstBlend = dst;
			} else {
				if ( gl->blend )
					glDisable( GL_BLEND );
			}
			gl->blend = blend;
		}
	}

	// depth (JA convention: standard depth, LEQUAL, clear depth 1.0)
	{
		qboolean depthTest = ( state_bits & GLS_DEPTHTEST_DISABLE ) ? qfalse : qtrue;
		qboolean depthWrite = ( state_bits & GLS_DEPTHMASK_TRUE ) ? qtrue : qfalse;
		GLenum depthFunc = ( state_bits & GLS_DEPTHFUNC_EQUAL ) ? GL_EQUAL : GL_LEQUAL;

		if ( gl->depthTest != depthTest ) {
			if ( depthTest )
				glEnable( GL_DEPTH_TEST );
			else
				glDisable( GL_DEPTH_TEST );
			gl->depthTest = depthTest;
		}
		if ( gl->depthWrite != depthWrite ) {
			glDepthMask( depthWrite ? GL_TRUE : GL_FALSE );
			gl->depthWrite = depthWrite;
		}
		if ( gl->depthFunc != depthFunc ) {
			glDepthFunc( depthFunc );
			gl->depthFunc = depthFunc;
		}
	}
}

static void g3_apply_cull( cullType_t cull_type, qboolean mirror )
{
	g3_glstate_t *gl = &g3.gl;
	qboolean cull = ( cull_type != CT_TWO_SIDED ) ? qtrue : qfalse;
	GLenum face;

	if ( !cull ) {
		if ( gl->cull ) {
			glDisable( GL_CULL_FACE );
			gl->cull = qfalse;
		}
		return;
	}

	if ( mirror ) {
		face = ( cull_type == CT_FRONT_SIDED ) ? GL_BACK : GL_FRONT;
	} else {
		face = ( cull_type == CT_FRONT_SIDED ) ? GL_FRONT : GL_BACK;
	}
	if ( !gl->cull ) {
		glEnable( GL_CULL_FACE );
		gl->cull = qtrue;
	}
	if ( gl->cullFace != face ) {
		glCullFace( face );
		gl->cullFace = face;
	}
	if ( gl->frontFace != GL_CCW ) {
		glFrontFace( GL_CCW );
		gl->frontFace = GL_CCW;
	}
}

static void g3_apply_offset( qboolean enabled )
{
	g3_glstate_t *gl = &g3.gl;

	if ( enabled ) {
		if ( !gl->polygonOffset ) {
			glEnable( GL_POLYGON_OFFSET_FILL );
			gl->polygonOffset = qtrue;
		}
		if ( gl->offsetFactor != r_offsetFactor->value || gl->offsetUnits != r_offsetUnits->value ) {
			glPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );
			gl->offsetFactor = r_offsetFactor->value;
			gl->offsetUnits = r_offsetUnits->value;
		}
	} else {
		if ( gl->polygonOffset ) {
			glDisable( GL_POLYGON_OFFSET_FILL );
			gl->polygonOffset = qfalse;
		}
	}
}

static void g3_apply_state( const g3_pipeline_def_t *def )
{
	g3_apply_blend_depth( def->state_bits );
	g3_apply_cull( def->cull_type, def->mirror ? qtrue : qfalse );
	g3_apply_offset( def->polygon_offset ? qtrue : qfalse );
}

// GL_State replacement for the backend: applies blend/depth bits through the
// state cache (cull and polygon offset are pipeline properties here).
void g3_state_bits( uint32_t state_bits )
{
	g3_apply_blend_depth( state_bits );
}

// GL_Cull replacement for the backend.
void g3_cull( cullType_t cull_type, bool mirror )
{
	g3_apply_cull( cull_type, mirror ? qtrue : qfalse );
}

void g3_init( void )
{
	memset( &g3.gl, 0, sizeof( g3.gl ) );
	g3.gl.depthFunc = GL_LEQUAL;
	g3.gl.depthWrite = qtrue;
	g3.gl.depthRangeNear = 0.0f;
	g3.gl.depthRangeFar = 1.0f;
	g3.gl.frontFace = GL_CCW;

	// force the GL context into the baseline described above
	glDisable( GL_BLEND );
	glDisable( GL_DEPTH_TEST );
	glDepthMask( GL_TRUE );
	glDepthFunc( GL_LEQUAL );
	glDisable( GL_CULL_FACE );
	glFrontFace( GL_CCW );
	glDisable( GL_POLYGON_OFFSET_FILL );
	glDepthRangef( 0.0f, 1.0f );

	g3.attr_enabled = 0;
	g3.attr_pending = 0;
	g3.buffer_mapped = qfalse;

	// NOTE: programs and the geometry buffer survive vid_restart (same
	// context); they are only lost when the context itself is recreated.
}

//
// MVP (no matrix stack in ES3: computed on CPU, plan section 1.1/2 point 5)
//

void g3_get_mvp( float *mvp )
{
	if ( backEnd.projection2D ) {
		// RB_SetGL2D equivalent: qglOrtho(0, 640, 480, 0, 0, 1) with
		// y=0 at the top of the 640x480 virtual screen (GL_Ortho from
		// gles3-quake3e tr_backend.c as model, flipped like vanilla).
		memset( mvp, 0, 16 * sizeof( float ) );
		mvp[0] = 2.0f / 640.0f;
		mvp[5] = -2.0f / 480.0f;
		mvp[10] = -2.0f;		// z: [0..1] -> [-1..1]
		mvp[12] = -1.0f;
		mvp[13] = 1.0f;
		mvp[14] = -1.0f;
		mvp[15] = 1.0f;
	} else {
		// vanilla loaded projectionMatrix as GL_PROJECTION and
		// backEnd.ori.modelMatrix as GL_MODELVIEW: P * MV.
		myGlMultMatrix( backEnd.viewParms.projectionMatrix, backEnd.ori.modelMatrix, mvp );
	}
}

static void g3_apply_mvp( g3_program_t *prog )
{
	float mvp[16];

	if ( prog->loc_u_mvp < 0 )
		return;

	g3_get_mvp( mvp );
	if ( memcmp( prog->cached_mvp, mvp, sizeof( mvp ) ) != 0 ) {
		glUniformMatrix4fv( prog->loc_u_mvp, 1, GL_FALSE, mvp );
		Com_Memcpy( prog->cached_mvp, mvp, sizeof( mvp ) );
	}
}

//
// Geometry streaming (gles3_map_geometry_buffer / gles3_flush_geometry model)
//

static uint32_t g3_pad( uint32_t v, uint32_t a )
{
	return ( v + ( a - 1 ) ) & ~( a - 1 );
}

static uint32_t g3_log2pad( uint32_t v )
{
	uint32_t r = 1;
	if ( v == 0 )
		return 0;
	v--;
	while ( v >>= 1 )
		r++;
	return 1u << r;
}

static void g3_resize_geometry_buffer( uint32_t new_size )
{
	if ( g3.geometry_buffer == 0 )
		glGenBuffers( 1, &g3.geometry_buffer );
	glBindBuffer( GL_ARRAY_BUFFER, g3.geometry_buffer );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, g3.geometry_buffer );
	glBufferData( GL_ARRAY_BUFFER, new_size, NULL, GL_DYNAMIC_DRAW );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, new_size, NULL, GL_DYNAMIC_DRAW );
	if ( g3.geometry_host_size < new_size ) {
		free( g3.geometry_host );
		g3.geometry_host = (byte *)malloc( new_size );
		g3.geometry_host_size = new_size;
	}
	g3.geometry_size = new_size;
	// full re-upload: everything written so far lives in the host shadow
	g3.geometry_uploaded = 0;
	ri.Printf( PRINT_DEVELOPER, "GLES3: geometry buffer resized to %u bytes\n", new_size );
}

static void g3_map_geometry_buffer( void )
{
	if ( g3.geometry_buffer == 0 ) {
		glGenVertexArrays( 1, &g3.vao );
		glBindVertexArray( g3.vao );
		g3.attr_enabled = 0;
		g3.attr_pending = 0;
		g3_resize_geometry_buffer( G3_VERTEX_BUFFER_SIZE );
	}

	// orphan the storage once per frame so per-draw uploads don't stall on
	// ranges still referenced by enqueued draws of the previous frame
	glBindBuffer( GL_ARRAY_BUFFER, g3.geometry_buffer );
	glBufferData( GL_ARRAY_BUFFER, g3.geometry_size, NULL, GL_STREAM_DRAW );

	g3.geometry_offset = 0;
	g3.geometry_uploaded = 0;
	g3.buffer_mapped = qtrue;
}

// append to the host shadow; returns offset or ~0u when out of space
static uint32_t g3_stream_write( const void *src, uint32_t size, uint32_t align )
{
	uint32_t offset = g3_pad( g3.geometry_offset, align );
	if ( offset + size > g3.geometry_size )
		return ~0u;
	Com_Memcpy( g3.geometry_host + offset, src, size );
	g3.geometry_offset = offset + size;
	return offset;
}

// upload the newly written range to the GL buffer (see gles3_flush_geometry:
// glMapBufferRange + UNSYNCHRONIZED avoids whole-buffer syncs on tile GPUs)
static void g3_flush_geometry( void )
{
	uint32_t end = g3.geometry_offset;
	if ( end > g3.geometry_uploaded ) {
		uint32_t offset = g3.geometry_uploaded;
		uint32_t size = end - offset;
		void *dst;

		glBindBuffer( GL_ARRAY_BUFFER, g3.geometry_buffer );
		dst = glMapBufferRange( GL_ARRAY_BUFFER, offset, size,
			GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT );
		if ( dst ) {
			Com_Memcpy( dst, g3.geometry_host + offset, size );
			glUnmapBuffer( GL_ARRAY_BUFFER );
		} else {
			glBufferSubData( GL_ARRAY_BUFFER, offset, size, g3.geometry_host + offset );
		}
		g3.geometry_uploaded = end;
	}
}

static void g3_set_attr( int index, int size, GLenum type, GLboolean normalized, GLsizei stride, uint32_t offset )
{
	uint32_t bit = 1u << index;
	glVertexAttribPointer( index, size, type, normalized, stride, (const void *)(uintptr_t)offset );
	g3.attr_pending |= bit;
}

static void g3_commit_attrs( void )
{
	uint32_t disable = g3.attr_enabled & ~g3.attr_pending;
	uint32_t bit;
	int i;

	for ( i = 0, bit = 1u; i < 32; i++, bit <<= 1 ) {
		if ( disable & bit )
			glDisableVertexAttribArray( i );
		if ( g3.attr_pending & bit )
			glEnableVertexAttribArray( i );
	}
	g3.attr_enabled = g3.attr_pending;
	g3.attr_pending = 0;
}

static g3_program_t *g3_bind_pipeline( const g3_pipeline_def_t *def )
{
	GLuint prog;
	int i;

	prog = g3_get_program( def );
	if ( prog == 0 )
		return NULL;

	if ( g3.gl.program != prog ) {
		glUseProgram( prog );
		g3.gl.program = prog;
	}

	g3_apply_state( def );

	for ( i = 0; i < g3.program_count; i++ ) {
		if ( g3.programs[i].program == prog )
			return &g3.programs[i];
	}
	return NULL;
}

// shared tail of the two public draw entries: program + MVP + attrs + flush + draw
static void g3_draw_commit( g3_program_t *prog, uint32_t indexOffset, int numIndexes, GLenum mode, int numVerts )
{
	if ( !prog )
		return;

	glActiveTexture( GL_TEXTURE0 );

	g3_apply_mvp( prog );
	g3_commit_attrs();
	g3_flush_geometry();

	if ( numIndexes > 0 ) {
		glDrawElements( mode, numIndexes, GL_UNSIGNED_INT, (const void *)(uintptr_t)indexOffset );
	} else {
		glDrawArrays( mode, 0, numVerts );
	}
}

void g3_draw_tess( const g3_pipeline_def_t *def, int numIndexes, const glIndex_t *indexes )
{
	g3_program_t *prog;
	uint32_t off_xyz, off_color, off_tc0, off_indexes;
	int i;

	if ( numIndexes <= 0 )
		return;

	if ( !tess.numVertexes )
		return;

	// vanilla fed tess.xyz through qglVertexPointer(3, ..., 16, xyz), so the
	// w component was never read; in ES3 the shader takes a vec4 and w must
	// be exactly 1 for the MVP to work
	if ( !g3.buffer_mapped )
		g3_map_geometry_buffer();

	// out of space with the current cursor: grow, orphan and restart the
	// frame segment. Draws already submitted this frame keep referencing the
	// previous (orphaned) storage, which is the standard orphaning idiom;
	// uploaded is reset so the new segment is uploaded in full on flush.
	if ( g3.geometry_offset + tess.numVertexes * ( 16 + 4 + 8 ) + numIndexes * 4 + 64 > g3.geometry_size ) {
		g3_resize_geometry_buffer( g3_log2pad( g3.geometry_offset + tess.numVertexes * ( 16 + 4 + 8 ) + numIndexes * 4 + 64 ) );
		g3_map_geometry_buffer();
	}

	glBindVertexArray( g3.vao );
	glBindBuffer( GL_ARRAY_BUFFER, g3.geometry_buffer );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, g3.geometry_buffer );

	off_xyz = g3_pad( g3.geometry_offset, 32 );
	if ( off_xyz + tess.numVertexes * sizeof( tess.xyz[0] ) > g3.geometry_size )
		return;
	// stream xyz manually so w can be forced to 1 (see above)
	{
		float *dst = (float *)( g3.geometry_host + off_xyz );
		for ( i = 0; i < tess.numVertexes; i++ ) {
			dst[i * 4 + 0] = tess.xyz[i][0];
			dst[i * 4 + 1] = tess.xyz[i][1];
			dst[i * 4 + 2] = tess.xyz[i][2];
			dst[i * 4 + 3] = 1.0f;
		}
		g3.geometry_offset = off_xyz + tess.numVertexes * sizeof( tess.xyz[0] );
	}
	off_color = g3_stream_write( tess.svars.colors, tess.numVertexes * sizeof( tess.svars.colors[0] ), 4 );
	off_tc0 = g3_stream_write( tess.svars.texcoords[0], tess.numVertexes * sizeof( tess.svars.texcoords[0][0] ), 4 );
	off_indexes = g3_stream_write( indexes, numIndexes * sizeof( indexes[0] ), 4 );
	if ( off_xyz == ~0u || off_color == ~0u || off_tc0 == ~0u || off_indexes == ~0u )
		return; // cannot happen: growth above guarantees space

	g3.attr_pending = g3.attr_enabled;
	g3_set_attr( 0, 4, GL_FLOAT, GL_FALSE, sizeof( tess.xyz[0] ), off_xyz );
	g3_set_attr( 1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( tess.svars.colors[0] ), off_color );
	g3_set_attr( 2, 2, GL_FLOAT, GL_FALSE, sizeof( tess.svars.texcoords[0][0] ), off_tc0 );

	prog = g3_bind_pipeline( def );
	g3_draw_commit( prog, off_indexes, numIndexes, GL_TRIANGLES, 0 );
}

void g3_draw_arrays( const g3_pipeline_def_t *def, GLenum mode, int numVerts,
		const float *xyz4, const byte *color4ub, const float *texcoord2 )
{
	g3_program_t *prog;
	uint32_t off_xyz, off_color, off_tc0;
	uint32_t need;

	if ( numVerts <= 0 )
		return;

	need = numVerts * ( 16 + 4 + 8 ) + 64;
	if ( !g3.buffer_mapped || g3.geometry_offset + need > g3.geometry_size ) {
		if ( g3.geometry_size < need )
			g3_resize_geometry_buffer( g3_log2pad( need ) );
		g3_map_geometry_buffer();
	}

	glBindVertexArray( g3.vao );
	glBindBuffer( GL_ARRAY_BUFFER, g3.geometry_buffer );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, g3.geometry_buffer );

	off_xyz = g3_pad( g3.geometry_offset, 32 );
	if ( off_xyz + numVerts * 4 * sizeof( float ) > g3.geometry_size )
		return;
	// same vec4/w story as in g3_draw_tess: force w to 1
	{
		const float *src = xyz4;
		float *dst = (float *)( g3.geometry_host + off_xyz );
		int i;
		for ( i = 0; i < numVerts; i++ ) {
			dst[i * 4 + 0] = src[i * 4 + 0];
			dst[i * 4 + 1] = src[i * 4 + 1];
			dst[i * 4 + 2] = src[i * 4 + 2];
			dst[i * 4 + 3] = 1.0f;
		}
		g3.geometry_offset = off_xyz + numVerts * 4 * sizeof( float );
	}
	off_color = g3_stream_write( color4ub, numVerts * 4, 4 );
	off_tc0 = ( texcoord2 != NULL ) ? g3_stream_write( texcoord2, numVerts * 2 * sizeof( float ), 4 ) : ~0u;

	g3.attr_pending = g3.attr_enabled;
	g3_set_attr( 0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof( float ), off_xyz );
	g3_set_attr( 1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4, off_color );
	if ( off_tc0 != ~0u )
		g3_set_attr( 2, 2, GL_FLOAT, GL_FALSE, 2 * sizeof( float ), off_tc0 );

	prog = g3_bind_pipeline( def );
	g3_draw_commit( prog, 0, 0, mode, numVerts );
}

// called by RB_SwapBuffers after WIN_Present: the next draw of the new frame
// orphans the geometry storage again
void g3_frame_end( void )
{
	g3.buffer_mapped = qfalse;
}
