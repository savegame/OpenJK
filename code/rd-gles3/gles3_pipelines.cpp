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

// Pipeline cache for rd-gles3, ported from rd-vulkan vk_pipelines.cpp.
//
// A "pipeline" is a (Vk_Pipeline_Def -> GL program) pair; all GL render
// state described by the def (blend, depth, cull, offset) is applied
// dynamically by gles3_set_state() at bind time, so programs are shared
// across state-bit variants of the same shader class.

#include "tr_local.h"

static qboolean gles3_def_equal( const Vk_Pipeline_Def *a, const Vk_Pipeline_Def *b )
{
	return	(a->state_bits == b->state_bits &&
			a->face_culling == b->face_culling &&
			a->polygon_offset == b->polygon_offset &&
			a->mirror == b->mirror &&
			a->shader_type == b->shader_type &&
			a->shadow_phase == b->shadow_phase &&
			a->primitives == b->primitives &&
			a->surface_sprite_flags == b->surface_sprite_flags &&
			a->line_width == b->line_width &&
			a->fog_stage == b->fog_stage &&
			a->abs_light == b->abs_light &&
			a->allow_discard == b->allow_discard &&
			a->acff == b->acff &&
			a->color.rgb == b->color.rgb &&
			a->color.alpha == b->color.alpha) ? qtrue : qfalse;
}

uint32_t vk_find_pipeline_ext( uint32_t base, const Vk_Pipeline_Def *def, qboolean use )
{
	uint32_t i;

	for ( i = base; i < vk.pipelines_count; i++ ) {
		if ( gles3_def_equal( &vk.pipelines[i].def, def ) )
			return i;
	}

	if ( !use && vk.pipelines_count >= MAX_VK_PIPELINES ) {
		ri.Error( ERR_DROP, "vk_find_pipeline_ext: pipeline cache exhausted" );
	}

	i = vk.pipelines_count++;
	vk.pipelines[i].def = *def;
	vk.pipelines[i].program = 0;

	return i;
}

void vk_get_pipeline_def( uint32_t pipeline, Vk_Pipeline_Def *def )
{
	if ( pipeline >= vk.pipelines_count ) {
		Com_Memset( def, 0, sizeof(*def) );
		return;
	}
	*def = vk.pipelines[pipeline].def;
}

static uint32_t gles3_create_std_pipeline( Vk_Shader_Type type, uint32_t state_bits, cullType_t cull, qboolean poly_offset )
{
	Vk_Pipeline_Def def;

	Com_Memset( &def, 0, sizeof(def) );
	def.shader_type = type;
	def.state_bits = state_bits;
	def.face_culling = cull;
	def.primitives = TRIANGLE_LIST;
	def.polygon_offset = poly_offset;

	return vk_find_pipeline_ext( 0, &def, qtrue );
}

void vk_create_pipelines( void )
{
	uint32_t i, j, k, l;

	vk.pipelines_count = 0;
	vk.pipelines_world_base = 0;

	//
	// standard pipelines
	//
	vk.std_pipeline.skybox_pipeline = gles3_create_std_pipeline( TYPE_COLOR_BLACK, GLS_DEFAULT, CT_FRONT_SIDED, qfalse );
	vk.std_pipeline.worldeffect_pipeline[0] = gles3_create_std_pipeline( TYPE_SINGLE_TEXTURE, GLS_DEFAULT, CT_FRONT_SIDED, qfalse );
	vk.std_pipeline.worldeffect_pipeline[1] = gles3_create_std_pipeline( TYPE_SINGLE_TEXTURE, GLS_DEFAULT, CT_TWO_SIDED, qfalse );

	// stencil shadow volumes: src=dst_alpha dst=one_minus_src_alpha style blending
	for ( i = 0; i < 2; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			vk.std_pipeline.shadow_volume_pipelines[i][j] = gles3_create_std_pipeline( TYPE_COLOR_BLACK,
				GLS_SRCBLEND_DST_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHTEST_DISABLE,
				i == 0 ? CT_FRONT_SIDED : CT_BACK_SIDED, qfalse );
		}
	}
	vk.std_pipeline.shadow_finish_pipeline = gles3_create_std_pipeline( TYPE_COLOR_BLACK,
		GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA | GLS_DSTBLEND_SRC_ALPHA | GLS_DEPTHTEST_DISABLE, CT_TWO_SIDED, qfalse );

	// fog pipelines: fogPass(3) x cull(3... stored as 2 + polygonOffset) x offset(2) x fogStage(2)
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 3; k++ ) {
				for ( l = 0; l < 2; l++ ) {
					vk.std_pipeline.fog_pipelines[i][j][k][l] = gles3_create_std_pipeline( TYPE_FOG_ONLY,
						GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHTEST_DISABLE,
						(cullType_t)j, l ? qtrue : qfalse );
				}
			}
		}
	}

#ifdef USE_PMLIGHT
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				for ( l = 0; l < 2; l++ ) {
					vk.std_pipeline.dlight_pipelines_x[i][j][k][l] = gles3_create_std_pipeline( TYPE_SINGLE_TEXTURE_LIGHTING,
						GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK_TRUE,
						(cullType_t)i, j ? qtrue : qfalse );
					vk.std_pipeline.dlight1_pipelines_x[i][j][k][l] = gles3_create_std_pipeline( TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR,
						GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK_TRUE,
						(cullType_t)i, j ? qtrue : qfalse );
				}
			}
		}
	}
#endif

	// debug visualization
	vk.std_pipeline.tris_debug_pipeline = gles3_create_std_pipeline( TYPE_COLOR_GREEN, GLS_POLYMODE_LINE | GLS_DEPTHTEST_DISABLE, CT_TWO_SIDED, qfalse );
	vk.std_pipeline.tris_mirror_debug_pipeline = vk.std_pipeline.tris_debug_pipeline;
	vk.std_pipeline.tris_debug_green_pipeline = vk.std_pipeline.tris_debug_pipeline;
	vk.std_pipeline.tris_mirror_debug_green_pipeline = vk.std_pipeline.tris_debug_pipeline;
	vk.std_pipeline.tris_debug_red_pipeline = gles3_create_std_pipeline( TYPE_COLOR_RED, GLS_POLYMODE_LINE | GLS_DEPTHTEST_DISABLE, CT_TWO_SIDED, qfalse );
	vk.std_pipeline.tris_mirror_debug_red_pipeline = vk.std_pipeline.tris_debug_red_pipeline;

	vk.std_pipeline.normals_debug_pipeline = vk.std_pipeline.tris_debug_pipeline;
	vk.std_pipeline.surface_debug_pipeline_solid = gles3_create_std_pipeline( TYPE_COLOR_WHITE, GLS_DEPTHTEST_DISABLE, CT_TWO_SIDED, qfalse );
	vk.std_pipeline.surface_debug_pipeline_outline = gles3_create_std_pipeline( TYPE_COLOR_BLACK, GLS_POLYMODE_LINE | GLS_DEPTHTEST_DISABLE, CT_TWO_SIDED, qfalse );
	vk.std_pipeline.images_debug_pipeline = gles3_create_std_pipeline( TYPE_SINGLE_TEXTURE, GLS_DEPTHTEST_DISABLE, CT_TWO_SIDED, qfalse );

	vk.std_pipeline.surface_beam_pipeline = gles3_create_std_pipeline( TYPE_SINGLE_TEXTURE, GLS_DEFAULT, CT_TWO_SIDED, qfalse );
	vk.std_pipeline.surface_axis_pipeline = gles3_create_std_pipeline( TYPE_SINGLE_TEXTURE, GLS_DEFAULT, CT_TWO_SIDED, qfalse );
	vk.std_pipeline.dot_pipeline = gles3_create_std_pipeline( TYPE_DOT, GLS_DEFAULT, CT_TWO_SIDED, qfalse );
}
