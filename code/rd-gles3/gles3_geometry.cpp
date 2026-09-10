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

// Tessellation -> draw-call translation for rd-gles3, ported from
// rd-vulkan vk_shade_geometry.cpp:
//   - RB_StageIteratorGeneric (per-stage pipeline selection & emission)
//   - ComputeColors (pure CPU color gen)
//   - RB_FogPass / fog & dlight uniform parameter calc
//   - geometry streaming into the VBO (vk_bind_geometry / vk_tess_index)
//
// Push constants and dynamic UBO offsets from the Vulkan version are
// replaced by a single CPU-side vkUniform_t block uploaded at draw time
// (uniform locations are resolved per program; MVP via u_MVP).

#include "tr_local.h"
#include "gles3_local.h"
#include "../rd-common/tr_common.h"

// vertex attribute layout, must match VS_GENERIC in gles3_shaders.cpp
#define ATTR_XYZ		0
#define ATTR_COLOR0		1
#define ATTR_ST0		2
#define ATTR_ST1		3
#define ATTR_ST2		4
#define ATTR_NORMAL		5
#define ATTR_COLOR1		6
#define ATTR_COLOR2		7
#define ATTR_COUNT		8

static float g_mvp[16];
static ss_input ssInput;
static qboolean g_mvp_valid = qfalse;
static GLenum g_prim_mode = GL_TRIANGLES;

void vk_select_texture( const int index )
{
	if (vk.ctmu == index)
		return;

	if ( index >= glConfig.maxActiveTextures )
		ri.Error(ERR_DROP, "%s: texture unit overflow = %i", __func__, index);

	vk.ctmu = index;
}

void vk_set_depthrange( const Vk_Depth_Range depthRange )
{
	tess.depthRange = depthRange;
}

static void get_mvp_transform( float *mvp )
{
	if (backEnd.projection2D)
	{
		float mvp0 = 2.0f / SCREEN_WIDTH;
		float mvp5 = 2.0f / SCREEN_HEIGHT;

		mvp[0] = mvp0; mvp[1] = 0.0f; mvp[2] = 0.0f; mvp[3] = 0.0f;
		// GL NDC y is up, virtual 2D screen y is down: flip (vk relied on viewport flip)
		mvp[4] = 0.0f; mvp[5] = -mvp5; mvp[6] = 0.0f; mvp[7] = 0.0f;
		// z_ndc = -1 (window depth 0, closest) so 2D passes LEQUAL vs the 1.0 clear
		mvp[8] = 0.0f; mvp[9] = 0.0f; mvp[10] = 2.0f; mvp[11] = 0.0f;
		mvp[12] = -1.0f; mvp[13] = 1.0f; mvp[14] = -1.0f; mvp[15] = 1.0f;
	}
	else
	{
		const float* p = backEnd.viewParms.projectionMatrix;
		float proj[16];
		Com_Memcpy(proj, p, 64);

		proj[5] = -p[5];
		myGlMultMatrix(vk_world.modelview_transform, proj, mvp);

		// The vk-style projection yields z_ndc in [0(near)..1(far)] for a
		// [0..1] viewport (rd-vulkan: clear 1.0, LESS_OR_EQUAL). GL window
		// depth is (z_ndc + 1) / 2, so expand z_clip' = 2*z_clip - w_clip to
		// map the same convention onto the GL depth buffer: near -> 0, far -> 1.
		{
			const float w0 = mvp[3], w1 = mvp[7], w2 = mvp[11], w3 = mvp[15];
			mvp[2]  = 2.0f * mvp[2]  - w0;
			mvp[6]  = 2.0f * mvp[6]  - w1;
			mvp[10] = 2.0f * mvp[10] - w2;
			mvp[14] = 2.0f * mvp[14] - w3;
		}
	}
}

void vk_update_mvp( const float *m ) {
	if (m)
		Com_Memcpy( g_mvp, m, sizeof(g_mvp) );
	else
		get_mvp_transform( g_mvp );

	g_mvp_valid = qtrue;
}

void vk_set_2d( void )
{
	if ( backEnd.projection2D ) {
		return;
	}

	backEnd.projection2D = qtrue;

	vk_update_mvp(NULL);

	// force depth range and viewport/scissor updates
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;

	// set time for 2D shaders
	backEnd.refdef.time = ri.Milliseconds() * ri.Cvar_VariableValue("timescale");
	backEnd.refdef.floatTime = (double)backEnd.refdef.time * 0.001;
}

// ---------------------------------------------------------------------------
// uniform staging
// ---------------------------------------------------------------------------

static byte *s_uniform_data;
static uint32_t s_uniform_size;
static uint32_t s_uniform_capacity;

uint32_t vk_append_uniform( const void *uniform, size_t size, uint32_t min_offset ) {
	uint32_t offset = PAD( s_uniform_size, MAX( min_offset, 16 ) );

	if ( offset + size > s_uniform_capacity ) {
		uint32_t new_capacity = s_uniform_capacity ? s_uniform_capacity * 2 : 65536;
		byte *new_data;
		while ( new_capacity < offset + size )
			new_capacity *= 2;
		new_data = (byte*)realloc( s_uniform_data, new_capacity );
		if ( new_data == NULL )
			return ~0U;
		s_uniform_data = new_data;
		s_uniform_capacity = new_capacity;
	}

	Com_Memcpy( s_uniform_data + offset, uniform, size );
	s_uniform_size = offset + (uint32_t)size;

	return offset;
}

void gles3_uniform_reset( void )
{
	s_uniform_size = 0;
}

void gles3_uniform_shutdown( void )
{
	if ( s_uniform_data ) {
		free( s_uniform_data );
		s_uniform_data = NULL;
	}
	s_uniform_size = 0;
	s_uniform_capacity = 0;
}

static uint32_t vk_push_uniform( const vkUniform_t *uniform )
{
	Com_Memcpy( &vk.uniform, uniform, sizeof(*uniform) );
	return 0;
}

void ForceAlpha(unsigned char *dstColors, int TR_ForceEntAlpha)
{
	int	i;

	dstColors += 3;

	for ( i = 0; i < tess.numVertexes; i++, dstColors += 4 )
	{
		*dstColors = TR_ForceEntAlpha;
	}
}

// Upload the current uniform block + MVP to the active program.
// g_cur_def is set by vk_bind_pipeline and carries the per-pipeline
// fixed color for the USE_FIXED_COLOR variants.
static const Vk_Pipeline_Def *g_cur_def;

void gles3_apply_uniforms( void )
{
	GLint loc;
	GLuint program;
	const vkUniform_t *u = &vk.uniform;

	glGetIntegerv( 0x8B8D /*GL_CURRENT_PROGRAM*/, (GLint*)&program ); // GL_CURRENT_PROGRAM

	loc = glGetUniformLocation( program, "u_MVP" );
	if ( loc >= 0 )
		glUniformMatrix4fv( loc, 1, GL_FALSE, g_mvp );

	loc = glGetUniformLocation( program, "u_ModelMatrix" );
	if ( loc >= 0 )
		glUniformMatrix4fv( loc, 1, GL_FALSE, u->modelMatrix );

	loc = glGetUniformLocation( program, "u_EyePos" );
	if ( loc >= 0 )
		glUniform4fv( loc, 1, u->eyePos );

	loc = glGetUniformLocation( program, "u_LightPos" );
	if ( loc >= 0 )
		glUniform4fv( loc, 1, u->lightPos );

	loc = glGetUniformLocation( program, "u_LightColor" );
	if ( loc >= 0 )
		glUniform4fv( loc, 1, u->lightColor );

	loc = glGetUniformLocation( program, "u_LightVector" );
	if ( loc >= 0 )
		glUniform4fv( loc, 1, u->lightVector );

	loc = glGetUniformLocation( program, "u_FogDistanceVector" );
	if ( loc >= 0 )
		glUniform4fv( loc, 1, u->fog.fogDistanceVector );

	loc = glGetUniformLocation( program, "u_FogDepthVector" );
	if ( loc >= 0 )
		glUniform4fv( loc, 1, u->fog.fogDepthVector );

	loc = glGetUniformLocation( program, "u_FogEyeT" );
	if ( loc >= 0 )
		glUniform4fv( loc, 1, u->fog.fogEyeT );

	loc = glGetUniformLocation( program, "u_FogColor" );
	if ( loc >= 0 )
		glUniform4fv( loc, 1, u->fog.fogColor );

	// texture unit assignments: bundle0..2 -> 0..2, fog -> 3
	loc = glGetUniformLocation( program, "u_Texture0" );
	if ( loc >= 0 )
		glUniform1i( loc, 0 );

	loc = glGetUniformLocation( program, "u_Texture1" );
	if ( loc >= 0 )
		glUniform1i( loc, 1 );

	loc = glGetUniformLocation( program, "u_Texture2" );
	if ( loc >= 0 )
		glUniform1i( loc, 2 );

	loc = glGetUniformLocation( program, "u_TextureFog" );
	if ( loc >= 0 )
		glUniform1i( loc, 3 );

	loc = glGetUniformLocation( program, "u_FixedColor" );
	if ( loc >= 0 ) {
		if ( g_cur_def ) {
			glUniform4f( loc, g_cur_def->color.rgb / 255.0f, g_cur_def->color.rgb / 255.0f,
						 g_cur_def->color.rgb / 255.0f, g_cur_def->color.alpha / 255.0f );
		} else {
			glUniform4f( loc, 1.0f, 1.0f, 1.0f, 1.0f );
		}
	}

	// alpha test threshold (u_AlphaTest), matching the vk specialization values
	loc = glGetUniformLocation( program, "u_AlphaTest" );
	if ( loc >= 0 ) {
		float value = 0.0f;
		if ( g_cur_def ) {
			switch ( g_cur_def->state_bits & GLS_ATEST_BITS ) {
				case GLS_ATEST_GT_0:	value = 0.0f; break;
				case GLS_ATEST_LT_80:	value = 0.5f; break;
				case GLS_ATEST_GE_80:	value = 0.5f; break;
				case GLS_ATEST_GE_C0:	value = 0.75f; break;
				default: break;
			}
		}
		glUniform1f( loc, value );
	}
}

// ---------------------------------------------------------------------------
// index streaming
// ---------------------------------------------------------------------------

uint32_t vk_tess_index( uint32_t numIndexes, const void *src )
{
	uint32_t offset;
	uint32_t size = numIndexes * sizeof(uint32_t);
	byte *dst = gles3_index_buffer_map( &offset, size );

	if ( dst == NULL )
		return ~0U;

	Com_Memcpy( dst, src, size );

	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, vk.index_buffer );
	glBufferSubData( GL_ELEMENT_ARRAY_BUFFER, offset, size, dst );

	vk.cmd->num_indexes = numIndexes;
	vk.cmd->index_offset = offset;

	return offset;
}

void vk_bind_index_ext( const int numIndexes, const uint32_t *indexes )
{
	vk_tess_index( (uint32_t)numIndexes, indexes );
}

void vk_bind_index( void )
{
	vk_bind_index_ext( tess.numIndexes, tess.indexes );
}

// ---------------------------------------------------------------------------
// geometry streaming
// ---------------------------------------------------------------------------

typedef struct gles3_geometry_s {
	uint32_t	offset[ATTR_COUNT];	// device offsets for the current batch
	uint32_t	enabled;			// attrib enable mask for the current batch
} gles3_geometry_t;

static gles3_geometry_t g_geom;

void vk_bind_geometry( uint32_t flags )
{
	uint32_t numVertexes = tess.numVertexes;
	uint32_t color_size = numVertexes * sizeof(color4ub_t);
	uint32_t xyz_size = numVertexes * sizeof(tess.xyz[0]);
	uint32_t st_size = numVertexes * sizeof(vec2_t);
	uint32_t nnn_size = numVertexes * sizeof(tess.normal[0]);
	uint32_t mask = 0;
	byte *ptr;

	if ( gles3_geometry_buffer_overflow() )
		return;

	// xyz
	ptr = gles3_geometry_buffer_map( &g_geom.offset[ATTR_XYZ], xyz_size );
	if ( ptr == NULL ) return;
	Com_Memcpy( ptr, tess.xyz, xyz_size );
	glBufferSubData( GL_ARRAY_BUFFER, g_geom.offset[ATTR_XYZ], xyz_size, ptr );
	mask |= 1 << ATTR_XYZ;

	// colors
	if ( flags & (TESS_RGBA0|TESS_RGBA1|TESS_RGBA2) ) {
		uint32_t off;
		ptr = gles3_geometry_buffer_map( &off, 3 * color_size );
		if ( ptr == NULL ) return;

		if ( flags & TESS_RGBA0 ) {
			Com_Memcpy( ptr, tess.svars.colors[0], color_size );
			g_geom.offset[ATTR_COLOR0] = off;
			mask |= 1 << ATTR_COLOR0;
		}
		if ( flags & TESS_RGBA1 ) {
			Com_Memcpy( ptr + color_size, tess.svars.colors[1], color_size );
			g_geom.offset[ATTR_COLOR1] = off + color_size;
			mask |= 1 << ATTR_COLOR1;
		}
		if ( flags & TESS_RGBA2 ) {
			Com_Memcpy( ptr + 2 * color_size, tess.svars.colors[2], color_size );
			g_geom.offset[ATTR_COLOR2] = off + 2 * color_size;
			mask |= 1 << ATTR_COLOR2;
		}
		glBufferSubData( GL_ARRAY_BUFFER, off, 3 * color_size, ptr );
	}

	// texcoords
	if ( flags & (TESS_ST0|TESS_ST1|TESS_ST2) ) {
		uint32_t off;
		ptr = gles3_geometry_buffer_map( &off, 3 * st_size );
		if ( ptr == NULL ) return;

		if ( flags & TESS_ST0 ) {
			Com_Memcpy( ptr, tess.svars.texcoordPtr[0], st_size );
			g_geom.offset[ATTR_ST0] = off;
			mask |= 1 << ATTR_ST0;
		}
		if ( flags & TESS_ST1 ) {
			Com_Memcpy( ptr + st_size, tess.svars.texcoordPtr[1], st_size );
			g_geom.offset[ATTR_ST1] = off + st_size;
			mask |= 1 << ATTR_ST1;
		}
		if ( flags & TESS_ST2 ) {
			Com_Memcpy( ptr + 2 * st_size, tess.svars.texcoordPtr[2], st_size );
			g_geom.offset[ATTR_ST2] = off + 2 * st_size;
			mask |= 1 << ATTR_ST2;
		}
		glBufferSubData( GL_ARRAY_BUFFER, off, 3 * st_size, ptr );
	}

	// normals
	if ( flags & TESS_NNN ) {
		ptr = gles3_geometry_buffer_map( &g_geom.offset[ATTR_NORMAL], nnn_size );
		if ( ptr == NULL ) return;
		Com_Memcpy( ptr, tess.normal, nnn_size );
		glBufferSubData( GL_ARRAY_BUFFER, g_geom.offset[ATTR_NORMAL], nnn_size, ptr );
		mask |= 1 << ATTR_NORMAL;
	}

	g_geom.enabled = mask;
}

void vk_bind_lighting( int stage, int bundle )
{
	// PMLIGHT dlight stage: xyz + st0 + normals
	uint32_t numVertexes = tess.numVertexes;
	uint32_t st_size = numVertexes * sizeof(vec2_t);
	uint32_t nnn_size = numVertexes * sizeof(tess.normal[0]);
	uint32_t mask = 0;
	byte *ptr;

	(void)stage;

	if ( gles3_geometry_buffer_overflow() )
		return;

	ptr = gles3_geometry_buffer_map( &g_geom.offset[ATTR_XYZ], numVertexes * sizeof(tess.xyz[0]) );
	if ( ptr == NULL ) return;
	Com_Memcpy( ptr, tess.xyz, numVertexes * sizeof(tess.xyz[0]) );
	glBufferSubData( GL_ARRAY_BUFFER, g_geom.offset[ATTR_XYZ], numVertexes * sizeof(tess.xyz[0]), ptr );
	mask |= 1 << ATTR_XYZ;

	// st0
	{
		uint32_t off;
		ptr = gles3_geometry_buffer_map( &off, st_size );
		if ( ptr == NULL ) return;
		ComputeTexCoords( bundle, &tess.xstages[stage]->bundle[bundle] );
		Com_Memcpy( ptr, tess.svars.texcoordPtr[bundle], st_size );
		glBufferSubData( GL_ARRAY_BUFFER, off, st_size, ptr );
		g_geom.offset[ATTR_ST0] = off;
		mask |= 1 << ATTR_ST0;
	}

	// normals
	ptr = gles3_geometry_buffer_map( &g_geom.offset[ATTR_NORMAL], nnn_size );
	if ( ptr == NULL ) return;
	Com_Memcpy( ptr, tess.normal, nnn_size );
	glBufferSubData( GL_ARRAY_BUFFER, g_geom.offset[ATTR_NORMAL], nnn_size, ptr );
	mask |= 1 << ATTR_NORMAL;

	g_geom.enabled = mask;
}

// ---------------------------------------------------------------------------
// pipeline & draw
// ---------------------------------------------------------------------------

void vk_bind_pipeline( uint32_t pipeline )
{
	const Vk_Pipeline_Def *def;
	GLuint program;
	cullType_t cull;
	static qboolean s_colormask_off = qfalse;

	if ( pipeline >= vk.pipelines_count ) {
		ri.Error( ERR_DROP, "vk_bind_pipeline: invalid pipeline %u", pipeline );
	}

	def = &vk.pipelines[pipeline].def;
	g_cur_def = def;

	if ( vk.pipelines[pipeline].program == 0 ) {
		vk.pipelines[pipeline].program = gles3_get_program( def );
	}

	// primitive mode for vk_draw_geometry (ES3 has no glPolygonMode; the
	// wireframe debug pipelines use LINE_LIST like quake3e renderergles3)
	switch ( def->primitives ) {
		default:
		case TRIANGLE_LIST:	g_prim_mode = GL_TRIANGLES; break;
		case TRIANGLE_STRIP:	g_prim_mode = GL_TRIANGLE_STRIP; break;
		case LINE_LIST:		g_prim_mode = GL_LINES; break;
		case POINT_LIST:		g_prim_mode = GL_POINTS; break;
	}

	program = vk.pipelines[pipeline].program;
	glUseProgram( program );

	// rd-vulkan swaps the cull side for mirror pipelines (def->mirror)
	cull = def->face_culling;
	if ( def->mirror ) {
		if ( cull == CT_FRONT_SIDED )
			cull = CT_BACK_SIDED;
		else if ( cull == CT_BACK_SIDED )
			cull = CT_FRONT_SIDED;
	}
	gles3_set_state( def->state_bits, cull, def->polygon_offset );

	// Folded-in stage fog samples the fog texture on unit 3 (the frontend
	// binds per-bundle textures to units 0..2 only).
	if ( def->fog_stage && def->shader_type != TYPE_FOG_ONLY && tr.fogImage ) {
		glActiveTexture( GL_TEXTURE3 );
		glBindTexture( GL_TEXTURE_2D, G3_IMG_H( tr.fogImage->handle ) );
	}

	// Depth-fragment stages write depth only (ES 3.0 has no gl_FragDepth;
	// the FS does the alpha cutout and the color mask hides the dummy output).
	if ( def->shader_type == TYPE_SINGLE_TEXTURE_DF ) {
		if ( !s_colormask_off ) {
			glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
			s_colormask_off = qtrue;
		}
	} else if ( s_colormask_off ) {
		glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		s_colormask_off = qfalse;
	}

	glActiveTexture( GL_TEXTURE0 + vk.ctmu );
}

static void gles3_commit_attribs( void )
{
	static uint32_t s_enabled_mask;
	uint32_t i;
	uint32_t mask = g_geom.enabled;

	for ( i = 0; i < ATTR_COUNT; i++ ) {
		if ( mask & (1 << i) ) {
			if ( !( s_enabled_mask & (1 << i) ) ) {
				glEnableVertexAttribArray( i );
			}
		} else if ( s_enabled_mask & (1 << i) ) {
			glDisableVertexAttribArray( i );
		}
	}
	s_enabled_mask = mask;

	if ( mask & (1 << ATTR_XYZ) )
		glVertexAttribPointer( ATTR_XYZ, 4, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET( g_geom.offset[ATTR_XYZ] ) );
	if ( mask & (1 << ATTR_COLOR0) )
		glVertexAttribPointer( ATTR_COLOR0, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, BUFFER_OFFSET( g_geom.offset[ATTR_COLOR0] ) );
	if ( mask & (1 << ATTR_COLOR1) )
		glVertexAttribPointer( ATTR_COLOR1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, BUFFER_OFFSET( g_geom.offset[ATTR_COLOR1] ) );
	if ( mask & (1 << ATTR_COLOR2) )
		glVertexAttribPointer( ATTR_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, BUFFER_OFFSET( g_geom.offset[ATTR_COLOR2] ) );
	if ( mask & (1 << ATTR_ST0) )
		glVertexAttribPointer( ATTR_ST0, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET( g_geom.offset[ATTR_ST0] ) );
	if ( mask & (1 << ATTR_ST1) )
		glVertexAttribPointer( ATTR_ST1, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET( g_geom.offset[ATTR_ST1] ) );
	if ( mask & (1 << ATTR_ST2) )
		glVertexAttribPointer( ATTR_ST2, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET( g_geom.offset[ATTR_ST2] ) );
	if ( mask & (1 << ATTR_NORMAL) )
		glVertexAttribPointer( ATTR_NORMAL, 4, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET( g_geom.offset[ATTR_NORMAL] ) );
}

void vk_draw_geometry( Vk_Depth_Range depth_range, qboolean indexed )
{
	gles3_commit_attribs();
	gles3_update_depth_range( depth_range );
	gles3_apply_uniforms();

	if ( indexed && vk.cmd->num_indexes > 0 ) {
		glDrawElements( g_prim_mode, vk.cmd->num_indexes, GL_UNSIGNED_INT, BUFFER_OFFSET( vk.cmd->index_offset ) );
	} else {
		glDrawArrays( g_prim_mode, 0, tess.numVertexes );
	}
}

void vk_draw_dot( uint32_t storage_offset )
{
	(void)storage_offset;
}

#ifdef USE_PMLIGHT
void vk_lighting_pass( void );
#endif

// ---------------------------------------------------------------------------
// image binding
// ---------------------------------------------------------------------------

void ComputeTexCoords( const int b, const textureBundle_t *bundle ) {
	int	i;
	int tm;
	vec2_t* src, * dst;

	if (!tess.numVertexes)
		return;

	src = dst = tess.svars.texcoords[b];

	//
	// generate the texture coordinates
	//
	switch (bundle->tcGen)
	{
	case TCGEN_IDENTITY:
		src = tess.texCoords00;
		break;
	case TCGEN_TEXTURE:
		src = tess.texCoords[0];
		break;
	case TCGEN_LIGHTMAP:
		src = tess.texCoords[1];
		break;
	case TCGEN_LIGHTMAP1:
		src = tess.texCoords[2];
		break;
	case TCGEN_LIGHTMAP2:
		src = tess.texCoords[3];
		break;
	case TCGEN_LIGHTMAP3:
		src = tess.texCoords[4];
		break;
	case TCGEN_VECTOR:
		for (i = 0; i < tess.numVertexes; i++) {
			dst[i][0] = DotProduct(tess.xyz[i], bundle->tcGenVectors[0]);
			dst[i][1] = DotProduct(tess.xyz[i], bundle->tcGenVectors[1]);
		}
		break;
	case TCGEN_FOG:
		RB_CalcFogTexCoords((float*)dst);
		break;
	case TCGEN_ENVIRONMENT_MAPPED:
		RB_CalcEnvironmentTexCoords((float*)dst);
		break;
	case TCGEN_BAD:
		return;
	}

	//
	// alter texture coordinates
	//
	for (tm = 0; tm < bundle->numTexMods; tm++) {

		switch (bundle->texMods[tm].type)
		{
		case TMOD_NONE:
			tm = TR_MAX_TEXMODS; // break out of for loop
			break;

		case TMOD_TURBULENT:
			RB_CalcTurbulentTexCoords(&bundle->texMods[tm].wave, (float*)src, (float*)dst);
			src = dst;
			break;

		case TMOD_ENTITY_TRANSLATE:
			RB_CalcScrollTexCoords(backEnd.currentEntity->e.shaderTexCoord, (float*)src, (float*)dst);
			src = dst;
			break;

		case TMOD_SCROLL:
			RB_CalcScrollTexCoords(bundle->texMods[tm].translate, (float*)src, (float*)dst);
			src = dst;
			break;

		case TMOD_SCALE:
			RB_CalcScaleTexCoords(bundle->texMods[tm].translate, (float*)src, (float*)dst);
			src = dst;
			break;

		case TMOD_STRETCH:
			RB_CalcStretchTexCoords(&bundle->texMods[tm].wave, (float*)src, (float*)dst);
			src = dst;
			break;

		case TMOD_TRANSFORM:
			RB_CalcTransformTexCoords(&bundle->texMods[tm], (float*)src, (float*)dst);
			src = dst;
			break;

		case TMOD_ROTATE:
			RB_CalcRotateTexCoords(bundle->texMods[tm].translate[0], (float*)src, (float*)dst);
			src = dst;
			break;

		default:
			ri.Error(ERR_DROP, "ERROR: unknown texmod '%d' in shader '%s'", bundle->texMods[tm].type, tess.shader->name);
			break;
		}
	}

	tess.svars.texcoordPtr[b] = src;
}

void R_BindAnimatedImage( const textureBundle_t *bundle ) {

	int64_t index;

	if ( bundle->isVideoMap ) {
		ri.CIN_RunCinematic( bundle->videoMapHandle );
		ri.CIN_UploadCinematic( bundle->videoMapHandle );
		return;
	}
	if ( bundle->isScreenMap ) {
		if ( !backEnd.screenMapDone ) {
			vk_bind( tr.blackImage );
		}
		return;
	}

	if ( ( r_fullbright->value ) && bundle->isLightmap )
	{
		vk_bind( tr.whiteImage );
		return;
	}

	if ( bundle->numImageAnimations <= 1 ) {
		vk_bind(bundle->image[0]);
		return;
	}

#ifdef RF_SETANIMINDEX
	if ( backEnd.currentEntity->e.renderfx & RF_SETANIMINDEX )
	{
		index = backEnd.currentEntity->e.skinNum;
	}
	else
#endif
	{
		// it is necessary to do this messy calc to make sure animations line up
		// exactly with waveforms of the same frequency
		index = Q_ftol( tess.shaderTime * bundle->imageAnimationSpeed * FUNCTABLE_SIZE );
		index >>= FUNCTABLE_SIZE2;

		if ( index < 0 ) {
			index = 0;	// may happen with shader time offsets
		}
	}

	if ( bundle->oneShotAnimMap )
	{
		if ( index >= bundle->numImageAnimations )
		{
			// stick on last frame
			index = bundle->numImageAnimations - 1;
		}
	}
	else
	{
		// loop
		index %= bundle->numImageAnimations;
	}

	vk_bind( bundle->image[ index ] );
}

// ---------------------------------------------------------------------------
// color generation (pure CPU, ported from vk_shade_geometry.cpp)
// ---------------------------------------------------------------------------

void ComputeColors( const int b, color4ub_t *dest, const shaderStage_t *pStage, int forceRGBGen )
{
	int			i;
	qboolean killGen = qfalse;
	alphaGen_t forceAlphaGen = pStage->bundle[b].alphaGen;//set this up so we can override below

	if (!tess.numVertexes)
		return;

	if (tess.shader != tr.projectionShadowShader && tess.shader != tr.shadowShader &&
		(backEnd.currentEntity->e.renderfx & (RF_DISINTEGRATE1 | RF_DISINTEGRATE2)))
	{
		RB_CalcDisintegrateColors( (unsigned char*) dest );
		RB_CalcDisintegrateVertDeform();

		// We've done some custom alpha and color stuff, so we can skip the rest.  Let it do fog though
		killGen = qtrue;
	}

	if ( pStage->bundle[0].rgbGen == CGEN_LIGHTMAPSTYLE )
		forceRGBGen = CGEN_LIGHTMAPSTYLE;

	//
	// rgbGen
	//
	if (!forceRGBGen)
	{
		forceRGBGen = pStage->bundle[b].rgbGen;
	}

	// does not work for rotated models, technically, this should also be a CGEN type.
	// But that would entail adding new shader commands....which is too much work for one thing
	if (backEnd.currentEntity->e.renderfx & RF_VOLUMETRIC)
	{
		float* normal, dot;
		unsigned char* color;
		int			numVertexes;

		normal = tess.normal[0];
		color = tess.svars.colors[0][0];

		numVertexes = tess.numVertexes;

		for (i = 0; i < numVertexes; i++, normal += 4, color += 4)
		{
			dot = DotProduct(normal, backEnd.refdef.viewaxis[0]);

			dot *= dot * dot * dot;

			if (dot < 0.2f) // so low, so just clamp it
			{
				dot = 0.0f;
			}

			color[0] = color[1] = color[2] = color[3] = Q_ftol(backEnd.currentEntity->e.shaderRGBA[0] * (1 - dot));
		}

		killGen = qtrue;
	}

	if (killGen)
	{
		goto avoidGen;
	}

	//
	// rgbGen
	//
	switch (forceRGBGen)
	{
	case CGEN_IDENTITY:
		Com_Memset(dest, 0xff, tess.numVertexes * 4);
		break;
	default:
	case CGEN_IDENTITY_LIGHTING:
		Com_Memset(dest, tr.identityLightByte, tess.numVertexes * 4);
		break;
	case CGEN_LIGHTING_DIFFUSE:
		RB_CalcDiffuseColor( (unsigned char*) dest );
		break;
	case CGEN_LIGHTING_DIFFUSE_ENTITY:
		RB_CalcDiffuseEntityColor( (unsigned char*) dest );
		if (forceAlphaGen == AGEN_IDENTITY &&
			backEnd.currentEntity->e.shaderRGBA[3] == 0xff
			)
		{
			forceAlphaGen = AGEN_SKIP;	//already got it in this set since it does all 4 components
		}
		break;
	case CGEN_EXACT_VERTEX:
		Com_Memcpy(dest, tess.vertexColors, tess.numVertexes * sizeof(tess.vertexColors[0]));
		break;
	case CGEN_CONST:
		for (i = 0; i < tess.numVertexes; i++) {
			*(int *)dest[i] = *(int *)pStage->bundle[b].constantColor;
		}
		break;
	case CGEN_VERTEX:
		if (tr.identityLight == 1)
		{
			Com_Memcpy(dest, tess.vertexColors, tess.numVertexes * sizeof(tess.vertexColors[0]));
		}
		else
		{
			for ( i = 0; i < tess.numVertexes; i++ )
			{
				dest[i][0] = tess.vertexColors[i][0] * tr.identityLight;
				dest[i][1] = tess.vertexColors[i][1] * tr.identityLight;
				dest[i][2] = tess.vertexColors[i][2] * tr.identityLight;
				dest[i][3] = tess.vertexColors[i][3];
			}
		}
		break;
	case CGEN_ONE_MINUS_VERTEX:
		if (tr.identityLight == 1)
		{
			for (i = 0; i < tess.numVertexes; i++)
			{
				dest[i][0] = 255 - tess.vertexColors[i][0];
				dest[i][1] = 255 - tess.vertexColors[i][1];
				dest[i][2] = 255 - tess.vertexColors[i][2];
			}
		}
		else
		{
			for (i = 0; i < tess.numVertexes; i++)
			{
				dest[i][0] = (255 - tess.vertexColors[i][0]) * tr.identityLight;
				dest[i][1] = (255 - tess.vertexColors[i][1]) * tr.identityLight;
				dest[i][2] = (255 - tess.vertexColors[i][2]) * tr.identityLight;
			}
		}
		break;
	case CGEN_FOG:
	{
		const fog_t *fog;

		fog = tr.world->fogs + tess.fogNum;

		for (i = 0; i < tess.numVertexes; i++) {
			*(int *)&dest[i] = fog->colorInt;
		}
	}
	break;
	case CGEN_WAVEFORM:
		RB_CalcWaveColor(&pStage->bundle[b].rgbWave, (unsigned char*) dest);
		break;
	case CGEN_ENTITY:
		RB_CalcColorFromEntity( (unsigned char*) dest );
		if (forceAlphaGen == AGEN_IDENTITY && backEnd.currentEntity->e.shaderRGBA[3] == 0xff)
		{
			forceAlphaGen = AGEN_SKIP;	//already got it in this set since it does all 4 components
		}
		break;
	case CGEN_ONE_MINUS_ENTITY:
		RB_CalcColorFromOneMinusEntity( (unsigned char*) dest );
		break;
	case CGEN_LIGHTMAPSTYLE:
		for (i = 0; i < tess.numVertexes; i++)
		{
			*(int *)dest[i] = *(int *)styleColors[pStage->lightmapStyle[b%2]];
		}
		break;
	}

	//
	// alphaGen
	//
	switch ( forceAlphaGen )
	{
	case AGEN_SKIP:
		break;
	case AGEN_IDENTITY:
		if (forceRGBGen != CGEN_IDENTITY) {
			if ((forceRGBGen == CGEN_VERTEX && tr.identityLight != 1) ||
				forceRGBGen != CGEN_VERTEX) {
				for (i = 0; i < tess.numVertexes; i++) {
					dest[i][3] = 0xff;
				}
			}
		}
		break;
	case AGEN_CONST:
		if (forceRGBGen != CGEN_CONST) {
			for (i = 0; i < tess.numVertexes; i++) {
				dest[i][3] = pStage->bundle[b].constantColor[3];
			}
		}
		break;
	case AGEN_WAVEFORM:
		RB_CalcWaveAlpha(&pStage->bundle[b].alphaWave, (unsigned char*) dest );
		break;
	case AGEN_LIGHTING_SPECULAR:
		RB_CalcSpecularAlpha( (unsigned char*) dest );
		break;
	case AGEN_ENTITY:
		RB_CalcAlphaFromEntity( (unsigned char*) dest );
		break;
	case AGEN_ONE_MINUS_ENTITY:
		RB_CalcAlphaFromOneMinusEntity( (unsigned char*) dest );
		break;
	case AGEN_VERTEX:
		if (forceRGBGen != CGEN_VERTEX) {
			for (i = 0; i < tess.numVertexes; i++) {
				dest[i][3] = tess.vertexColors[i][3];
			}
		}
		break;
	case AGEN_ONE_MINUS_VERTEX:
		for (i = 0; i < tess.numVertexes; i++)
		{
			dest[i][3] = 255 - tess.vertexColors[i][3];
		}
		break;
	case AGEN_PORTAL:
	{
		for (i = 0; i < tess.numVertexes; i++)
		{
			unsigned char alpha;
			float len;
			vec3_t v;

			VectorSubtract(tess.xyz[i], backEnd.viewParms.ori.origin, v);
			len = VectorLength( v ) * tess.shader->portalRangeR;

			if ( len > 1 )
			{
				alpha = 0xff;
			}
			else
			{
				alpha = len * 0xff;
			}

			dest[i][3] = alpha;
		}
	}
	break;
	case AGEN_BLEND:
		if (forceRGBGen != CGEN_VERTEX)
		{
			for (i = 0; i < tess.numVertexes; i++)
			{
				dest[i][3] = tess.vertexAlphas[i][pStage->index]; //rwwRMG - added support
			}
		}
		break;
	default:
		break;
	}
avoidGen:
	//
	// fog adjustment for colors to fade out as fog increases
	//
	if (tess.fogNum)
	{
		switch (pStage->bundle[b].adjustColorsForFog)
		{
		case ACFF_MODULATE_RGB:
			RB_CalcModulateColorsByFog( (unsigned char*) dest );
			break;
		case ACFF_MODULATE_ALPHA:
			RB_CalcModulateAlphasByFog( (unsigned char*) dest );
			break;
		case ACFF_MODULATE_RGBA:
			RB_CalcModulateRGBAsByFog( (unsigned char*) dest );
			break;
		case ACFF_NONE:
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// fog
// ---------------------------------------------------------------------------

static vkUniform_t uniform;

const fogProgramParms_t *RB_CalcFogProgramParms( void )
{
	static fogProgramParms_t parm;
	const fog_t* fog;
	vec3_t		local;

	Com_Memset(parm.fogDepthVector, 0, sizeof(parm.fogDepthVector));

	fog = tr.world->fogs + tess.fogNum;

	// all fogging distance is based on world Z units
	VectorSubtract(backEnd.ori.origin, backEnd.viewParms.ori.origin, local);
	parm.fogDistanceVector[0] = -backEnd.ori.modelViewMatrix[2];
	parm.fogDistanceVector[1] = -backEnd.ori.modelViewMatrix[6];
	parm.fogDistanceVector[2] = -backEnd.ori.modelViewMatrix[10];
	parm.fogDistanceVector[3] = DotProduct(local, backEnd.viewParms.ori.axis[0]);

	// scale the fog vectors based on the fog's thickness
	parm.fogDistanceVector[0] *= fog->tcScale;
	parm.fogDistanceVector[1] *= fog->tcScale;
	parm.fogDistanceVector[2] *= fog->tcScale;
	parm.fogDistanceVector[3] *= fog->tcScale;

	// rotate the gradient vector for this orientation
	if (fog->hasSurface) {
		parm.fogDepthVector[0] = fog->surface[0] * backEnd.ori.axis[0][0] +
			fog->surface[1] * backEnd.ori.axis[0][1] + fog->surface[2] * backEnd.ori.axis[0][2];
		parm.fogDepthVector[1] = fog->surface[0] * backEnd.ori.axis[1][0] +
			fog->surface[1] * backEnd.ori.axis[1][1] + fog->surface[2] * backEnd.ori.axis[1][2];
		parm.fogDepthVector[2] = fog->surface[0] * backEnd.ori.axis[2][0] +
			fog->surface[1] * backEnd.ori.axis[2][1] + fog->surface[2] * backEnd.ori.axis[2][2];
		parm.fogDepthVector[3] = -fog->surface[3] + DotProduct(backEnd.ori.origin, fog->surface);

		parm.eyeT = DotProduct(backEnd.ori.viewOrigin, parm.fogDepthVector) + parm.fogDepthVector[3];
	}
	else {
		parm.eyeT = 1.0f; // non-surface fog always has eye inside
	}

	// see if the viewpoint is outside
	// this is needed for clipping distance even for constant fog
	if (parm.eyeT < 0) {
		parm.eyeOutside = qtrue;
	}
	else {
		parm.eyeOutside = qfalse;
	}

	parm.fogDistanceVector[3] += 1.0 / 512;
	parm.fogColor = fog->color;

	return &parm;
}

static void vk_set_fog_params( vkUniform_t *uniform, int *fogStage )
{
	if (tess.fogNum && tess.shader->fogPass) {
		const fogProgramParms_t *fp = RB_CalcFogProgramParms();
		// vertex data
		VectorCopy4(fp->fogDistanceVector, uniform->fog.fogDistanceVector);
		VectorCopy4(fp->fogDepthVector, uniform->fog.fogDepthVector);
		uniform->fog.fogEyeT[0] = fp->eyeT;
		if (fp->eyeOutside) {
			uniform->fog.fogEyeT[1] = 0.0; // fog eye out
		}
		else {
			uniform->fog.fogEyeT[1] = 1.0; // fog eye in
		}
		// fragment data
		if ( backEnd.isGlowPass )
			VectorCopy4( colorBlack, uniform->fog.fogColor );
		else
			VectorCopy4( fp->fogColor, uniform->fog.fogColor );

		*fogStage = 1;
	}
	else {
		*fogStage = 0;
	}
}

/*
===================
RB_FogPass
Blends a fog texture on top of everything else
===================
*/
static void RB_FogPass( void ) {
	uint32_t pipeline = vk.std_pipeline.fog_pipelines[0][tess.shader->fogPass - 1][tess.shader->cullType][tess.shader->polygonOffset];
	int fog_stage;

	vk_bind_pipeline( pipeline );
	vk_set_fog_params( &uniform, &fog_stage );

	if ( vk.hw_fog
		&& backEnd.currentEntity && !( backEnd.currentEntity == &backEnd.entity2D || backEnd.currentEntity == &tr.worldEntity ) )
	{
		trRefEntity_t *refEntity = backEnd.currentEntity;
		orientationr_t ori;

		uniform.fog.fogDistanceVector[3] = 1;	// is_entity

		R_RotateForEntity( refEntity, &backEnd.viewParms, &ori );
		Matrix16Copy( ori.modelMatrix, uniform.modelMatrix );
	}

	vk_push_uniform( &uniform );

	vk_select_texture( 0 );
	vk_bind( tr.fogImage );
	vk_draw_geometry( DEPTH_RANGE_NORMAL, qtrue );
}

// ---------------------------------------------------------------------------
// dynamic lighting (PMLIGHT)
// ---------------------------------------------------------------------------

#ifdef USE_PMLIGHT
static void vk_set_light_params( vkUniform_t *uniform, const dlight_t *dl ) {
	float radius;

	if (!glConfig.deviceSupportsGamma && !vk.fboActive)
		VectorScale(dl->color, 2 * powf(r_intensity->value, r_gamma->value), uniform->lightColor);
	else
		VectorCopy(dl->color, uniform->lightColor);

	radius = dl->radius;

	// vertex data
	VectorCopy(backEnd.ori.viewOrigin, uniform->eyePos); uniform->eyePos[3] = 0.0f;
	VectorCopy(dl->transformed, uniform->lightPos); uniform->lightPos[3] = 0.0f;

	// fragment data
	uniform->lightColor[3] = 1.0f / Square(radius);

	if (dl->linear)
	{
		vec4_t ab;
		VectorSubtract(dl->transformed2, dl->transformed, ab);
		ab[3] = 1.0f / DotProduct(ab, ab);
		VectorCopy4(ab, uniform->lightVector);
	}
}

void vk_lighting_pass( void )
{
	static uint32_t uniform_offset;
	static int fog_stage;
	uint32_t pipeline;
	const shaderStage_t *pStage;
	cullType_t cull;
	int abs_light;

	if (tess.shader->lightingStage < 0)
		return;

	pStage = tess.xstages[tess.shader->lightingStage];

	// we may need to update programs for fog transitions
	if (tess.dlightUpdateParams) {
		vk_set_fog_params(&uniform, &fog_stage);
		vk_set_light_params(&uniform, tess.light);

		uniform_offset = vk_push_uniform(&uniform);

		tess.dlightUpdateParams = qfalse;
	}

	if (uniform_offset == ~0)
		return; // no space left...

	cull = tess.shader->cullType;
	if (backEnd.viewParms.portalView == PV_MIRROR) {
		switch (cull) {
		case CT_FRONT_SIDED: cull = CT_BACK_SIDED; break;
		case CT_BACK_SIDED: cull = CT_FRONT_SIDED; break;
		default: break;
		}
	}

	abs_light = (cull == CT_TWO_SIDED) ? 1 : 0;

	if (tess.light->linear)
		pipeline = vk.std_pipeline.dlight1_pipelines_x[cull][tess.shader->polygonOffset][fog_stage][abs_light];
	else
		pipeline = vk.std_pipeline.dlight_pipelines_x[cull][tess.shader->polygonOffset][fog_stage][abs_light];

	vk_select_texture(0);
	R_BindAnimatedImage(&pStage->bundle[tess.shader->lightingBundle]);

	ComputeTexCoords(tess.shader->lightingBundle, &pStage->bundle[tess.shader->lightingBundle]);

	vk_bind_pipeline(pipeline);
	vk_bind_index();
	vk_bind_lighting(tess.shader->lightingStage, tess.shader->lightingBundle);
	vk_draw_geometry(tess.depthRange, qtrue);
}
#endif // USE_PMLIGHT

// ---------------------------------------------------------------------------
// the generic stage iterator (ported from vk_shade_geometry.cpp)
// ---------------------------------------------------------------------------

void RB_StageIteratorGeneric( void )
{
	const shaderStage_t		*pStage;
	Vk_Pipeline_Def			def;
	uint32_t				stage = 0;
	uint32_t				pipeline;
	int						tess_flags, i;
	int						fog_stage = 0;
	qboolean				fogCollapse;
	qboolean				push_uniform;

	RB_DeformTessGeometry();

	tess_flags = tess.shader->tessFlags;

	fogCollapse = qfalse;
	push_uniform = qfalse;

#ifdef USE_FOG_COLLAPSE
	if ( tess.fogNum && tess.shader->fogPass && tess.shader->fogCollapse && r_drawfog->value >= 2 ) {
		fogCollapse = qtrue;
	}
#endif

	vk_bind_index();

	if ( fogCollapse ) {
		vk_set_fog_params( &uniform, &fog_stage );
		VectorCopy( backEnd.ori.viewOrigin, uniform.eyePos );
		vk_select_texture( 1 );
		vk_bind( tr.fogImage );
		push_uniform = qtrue;
	}
	else {
		fog_stage = 0;
		if ( tess_flags & TESS_VPOS ) {
			VectorCopy( backEnd.ori.viewOrigin, uniform.eyePos );
			tess_flags &= ~TESS_VPOS;
			push_uniform = qtrue;
		}
	}

	for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ )
	{
		int			forceRGBGen = 0;
		qboolean	is_refraction = qfalse;

		pStage = tess.xstages[stage];

		if ( !pStage || !pStage->active )
			break;

		// we check for surfacesprites AFTER drawing everything else
		if ( pStage->ss && pStage->ss->type )
			continue;

		// vertexLightmap isnt used rn
		if ( stage && r_lightmap->integer && !( pStage->bundle[0].isLightmap || pStage->bundle[1].isLightmap || pStage->bundle[0].vertexLightmap ) )
			break;

		if ( backEnd.currentEntity ) {
			assert( backEnd.currentEntity->e.renderfx >= 0 );
			if ( backEnd.currentEntity->e.renderfx & RF_RGB_TINT )
				forceRGBGen = CGEN_ENTITY;
		}

		tess_flags |= pStage->tessFlags;

		// refraction
		if ( tess.shader->useDistortion == qtrue )
		{
			is_refraction = qtrue;
		}

		for ( i = 0; i < pStage->numTexBundles; i++ ) {
			if ( pStage->bundle[i].image[0] != NULL)  {
				vk_select_texture(i);

				if ( backEnd.isGlowPass )
				{
					// use blackimage for non glow bundles during a glowPass
					if ( !pStage->bundle[i].glow )
					{
						vk_bind( tr.blackImage );
						Com_Memset( tess.svars.colors[i], 0xff, tess.numVertexes * 4 );
						continue;
					}

					// edge case: ensure tessflags bits are set, could be optimized out if equalTC or equalRGB in
					// tr_shader: try to avoid redundant per-stage computations.
					// could result in stale tc or rgb data.
					if ( stage && !tess.xstages[stage -1]->bundle[i].glow && !(tess_flags & TESS_ENV) )
						tess_flags |= TESS_RGBA0 | TESS_ST0;
				}

				R_BindAnimatedImage(&pStage->bundle[i]);

				if ( tess_flags & (TESS_ST0 << i) )
					ComputeTexCoords( i, &pStage->bundle[i] );


				if ( (tess_flags & (TESS_RGBA0 << i)) || forceRGBGen )
					ComputeColors( i, tess.svars.colors[i], pStage, forceRGBGen );
			}
		}

		// reject this stage if it's not a glow stage but we are doing a glow pass.
		if ( backEnd.isGlowPass && !pStage->glow )
			continue;

		vk_select_texture( 0 );

		if ( r_lightmap->integer && pStage->bundle[1].isLightmap ) {
			vk_bind( tr.whiteImage ); // replace diffuse texture with a white one thus effectively render only lightmap
		}

		if ( backEnd.viewParms.portalView == PV_MIRROR ) {
			pipeline = pStage->vk_mirror_pipeline[fog_stage];
		}
		else {
			pipeline = pStage->vk_pipeline[fog_stage];
		}

		// for 2D flipped images
		if ( backEnd.projection2D ) {
			if ( !pStage->vk_2d_pipeline ) {
				vk_get_pipeline_def(pStage->vk_pipeline[0], &def);

				// use an existing pipeline with the same def or create a new one.
				def.face_culling = CT_TWO_SIDED;
				tess.xstages[stage]->vk_2d_pipeline = vk_find_pipeline_ext(0, &def, qfalse);
			}

			pipeline = pStage->vk_2d_pipeline;
		}
		else if ( backEnd.currentEntity ) {
			vk_get_pipeline_def(pipeline, &def);

			// we want to be able to rip a hole in the thing being disintegrated,
			// and by doing the depth-testing it avoids some kinds of artefacts, but will probably introduce others?
			if ( backEnd.currentEntity->e.renderfx & RF_DISINTEGRATE1 )
				def.state_bits = GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK_TRUE | GLS_ATEST_GE_C0;

			// only force blend on the internal distortion shader
			if ( tess.shader == tr.distortionShader )
				def.state_bits = GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK_TRUE;

			if ( backEnd.currentEntity->e.renderfx & RF_FORCE_ENT_ALPHA ) {
				ForceAlpha( (unsigned char *) tess.svars.colors, backEnd.currentEntity->e.shaderRGBA[3] );

				def.state_bits = GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;

				// depth write, so faces through the model will be stomped over by nearer ones.
#ifdef RF_ALPHA_DEPTH
				if ( backEnd.currentEntity->e.renderfx & RF_ALPHA_DEPTH )
					def.state_bits |= GLS_DEPTHMASK_TRUE;
#endif
			}

			//want to use RGBGen from ent
			// "forceRGBGen override" requires +cl glsl shader, substitute if identity shader is set.
			if ( forceRGBGen && !(tess_flags & TESS_RGBA0) )
			{
				tess_flags |= TESS_RGBA0;
				def.shader_type = !pStage->mtEnv ? TYPE_SINGLE_TEXTURE :
					( (def.shader_type >= TYPE_MULTI_TEXTURE_MUL2_IDENTITY) ? TYPE_MULTI_TEXTURE_MUL2 : TYPE_MULTI_TEXTURE_ADD2);
			}

			if ( is_refraction )
			{
				def.shader_type = TYPE_REFRACTION;
				def.face_culling = CT_TWO_SIDED;

				tess_flags |= TESS_NNN;
			}

			pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
		}

		qboolean set_model_matrix = qfalse;

		if ( is_refraction )
		{
			Com_Memset( &uniform.refraction, 0, sizeof(uniform.refraction) );

			set_model_matrix = qtrue;

			push_uniform = qtrue;
		}

		if ( !vk.hw_fog && fogCollapse
			&& backEnd.currentEntity && !( backEnd.currentEntity == &backEnd.entity2D || backEnd.currentEntity == &tr.worldEntity ) )
		{
			set_model_matrix = qtrue;
			uniform.fog.fogDistanceVector[3] = 1;	// is_entity
		}

		// when model vbos are disabled, but the shader still requires
		// the modelmatrix (fog or refraction) to get world space positions.
		// store the modelmatrix in main uniform
		if ( set_model_matrix )
		{
			trRefEntity_t *refEntity = backEnd.currentEntity;
			orientationr_t ori;

			R_RotateForEntity( refEntity, &backEnd.viewParms, &ori );
			Matrix16Copy( ori.modelMatrix, uniform.modelMatrix );
		}

		if ( push_uniform ) {
			push_uniform = qfalse;
			vk_push_uniform( &uniform );
		}

		vk_bind_pipeline( pipeline );
		vk_bind_geometry( tess_flags );
		vk_draw_geometry( tess.depthRange, qtrue );

		if ( pStage->depthFragment ) {
			if ( backEnd.viewParms.portalView == PV_MIRROR )
				pipeline = pStage->vk_mirror_pipeline_df;
			else
				pipeline = pStage->vk_pipeline_df;

			vk_bind_pipeline( pipeline );
			vk_draw_geometry( tess.depthRange, qtrue );
		}

		// allow skipping out to show just lightmaps during development
		if ( r_lightmap->integer && ( pStage->bundle[0].isLightmap || pStage->bundle[1].isLightmap ) )
			break;

		tess_flags = 0;
	}

	if ( push_uniform )
		vk_push_uniform( &uniform );

	if (tess_flags) // fog-only shaders?
		vk_bind_geometry(tess_flags);

	// now do fog
	if (tr.world && r_drawfog->value && tess.fogNum && tess.shader->fogPass && !fogCollapse) {
		RB_FogPass();
	}

	// Now check for surfacesprites.
	if ( r_surfaceSprites->integer && !vk.vboWorldActive )
	{
		qboolean ssFound = qfalse;

		for (stage = 1; stage < tess.shader->numUnfoggedPasses; stage++)
		{
			pStage = tess.xstages[stage];

			if ( !pStage || !pStage->ss || !pStage->ss->type )
				continue;

			if (!ssFound) {
				// don't cringe, this is a temporary solution. but slow..
				// we are still reading from tess.xyz while also writing a group of surfacesprites to it.
				// which means the next group will read from garbaged surface data.
				// we duplicate the necessary tess data to ssInput and use that to read from.
				ssInput.numIndexes = tess.numIndexes;
				ssInput.numVertexes = tess.numVertexes;

				memcpy(ssInput.indexes, tess.indexes, sizeof(tess.indexes));
				memcpy(ssInput.xyz, tess.xyz, sizeof(tess.xyz));
				memcpy(ssInput.normal, tess.normal, sizeof(tess.normal));
				memcpy(ssInput.vertexColors, tess.vertexColors, sizeof(tess.vertexColors));

				ssFound = qtrue;
			}

			// Draw the surfacesprite
			RB_DrawSurfaceSprites( pStage, &ssInput );
		}
	}
}
