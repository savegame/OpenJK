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

void vk_alloc_persistent_pipelines( void )
{
	unsigned int state_bits;
	Vk_Pipeline_Def def;

	// Straight port of rd-vulkan vk_alloc_persistent_pipelines
	// (vk_pipelines.cpp:1935-2205).  The defs must match the Vulkan ones
	// field for field: tr_*/G2_* pick pipelines out of vk.std_pipeline by
	// index and rely on their shader_type / shadow_phase / blend state.

	// skybox
	{
		Com_Memset( &def, 0, sizeof(def) );
		def.shader_type = TYPE_SINGLE_TEXTURE_FIXED_COLOR;
		def.color.rgb = tr.identityLightByte;
		def.color.alpha = tr.identityLightByte;
		def.face_culling = CT_FRONT_SIDED;
		def.polygon_offset = qfalse;
		def.mirror = qfalse;

		vk.std_pipeline.skybox_pipeline = vk_find_pipeline_ext( 0, &def, qtrue );
	}

	// world effects (weather)
	{
		Com_Memset( &def, 0, sizeof(def) );
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.face_culling = CT_TWO_SIDED;
		def.polygon_offset = qfalse;
		def.mirror = qfalse;

		def.state_bits = GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
		vk.std_pipeline.worldeffect_pipeline[0] = vk_find_pipeline_ext( 0, &def, qtrue );

		def.state_bits = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
		vk.std_pipeline.worldeffect_pipeline[1] = vk_find_pipeline_ext( 0, &def, qtrue );
	}

	// Q3 stencil shadows
	{
		{
			cullType_t cull_types[2] = { CT_FRONT_SIDED, CT_BACK_SIDED };
			qboolean mirror_flags[2] = { qfalse, qtrue };
			int i, j;

			Com_Memset( &def, 0, sizeof(def) );
			def.polygon_offset = qfalse;
			def.state_bits = 0;
			def.shader_type = TYPE_SINGLE_TEXTURE;
			def.shadow_phase = SHADOW_EDGES;

			for ( i = 0; i < 2; i++ )
			{
				def.face_culling = cull_types[i];
				for ( j = 0; j < 2; j++ ) {
					def.mirror = mirror_flags[j];
					vk.std_pipeline.shadow_volume_pipelines[i][j] = vk_find_pipeline_ext( 0, &def, r_shadows->integer ? qtrue : qfalse );
				}
			}
		}

		{
			Com_Memset( &def, 0, sizeof(def) );
			def.face_culling = CT_FRONT_SIDED;
			def.polygon_offset = qfalse;
			def.state_bits = GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO;
			def.shader_type = TYPE_SINGLE_TEXTURE;
			def.mirror = qfalse;
			def.shadow_phase = SHADOW_FS_QUAD;
			def.primitives = TRIANGLE_STRIP;
			vk.std_pipeline.shadow_finish_pipeline = vk_find_pipeline_ext( 0, &def, r_shadows->integer ? qtrue : qfalse );
		}
	}

	// fog and dlights
	{
		unsigned int fog_state_bits[2] = {
			GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL, // fogPass == FP_EQUAL
			GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA // fogPass == FP_LE
		};
		qboolean polygon_offset[2] = { qfalse, qtrue };
		int i, j, k;
#ifdef USE_PMLIGHT
		int l;
#endif

		Com_Memset( &def, 0, sizeof(def) );
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.mirror = qfalse;

		for ( i = 0; i < 2; i++ )
		{
			unsigned fog_state = fog_state_bits[i];

			for ( j = 0; j < 3; j++ )
			{
				def.face_culling = (cullType_t)j;

				for ( k = 0; k < 2; k++ )
				{
					def.polygon_offset = polygon_offset[k];
#ifdef USE_FOG_ONLY
					def.shader_type = TYPE_FOG_ONLY;
#else
					def.shader_type = TYPE_SINGLE_TEXTURE;
#endif
					def.state_bits = fog_state;
					vk.std_pipeline.fog_pipelines[0][i][j][k] = vk_find_pipeline_ext( 0, &def, qtrue );
					// USE_VBO_GHOUL2 / USE_VBO_MDV are off in rd-gles3 (G1-G4),
					// so the model-VBO fog variants alias the CPU-tess one
					vk.std_pipeline.fog_pipelines[1][i][j][k] = vk.std_pipeline.fog_pipelines[0][i][j][k];
					vk.std_pipeline.fog_pipelines[2][i][j][k] = vk.std_pipeline.fog_pipelines[0][i][j][k];
				}
			}
		}

#ifdef USE_PMLIGHT
		def.state_bits = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL;
		for ( i = 0; i < 3; i++ ) { // cullType
			def.face_culling = (cullType_t)i;
			for ( j = 0; j < 2; j++ ) { // polygonOffset
				def.polygon_offset = polygon_offset[j];
				for ( k = 0; k < 2; k++ ) {
					def.fog_stage = k; // fogStage
					for ( l = 0; l < 2; l++ ) {
						def.abs_light = l;
						def.shader_type = TYPE_SINGLE_TEXTURE_LIGHTING;
						vk.std_pipeline.dlight_pipelines_x[i][j][k][l] = vk_find_pipeline_ext( 0, &def, qfalse );
						def.shader_type = TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR;
						vk.std_pipeline.dlight1_pipelines_x[i][j][k][l] = vk_find_pipeline_ext( 0, &def, qfalse );
					}
				}
			}
		}
		def.fog_stage = 0;
		def.abs_light = 0;
#endif // USE_PMLIGHT
	}

	// flare visibility test dot
	{
		Com_Memset( &def, 0, sizeof(def) );
		def.face_culling = CT_TWO_SIDED;
		def.shader_type = TYPE_DOT;
		def.primitives = POINT_LIST;
		vk.std_pipeline.dot_pipeline = vk_find_pipeline_ext( 0, &def, qtrue );
	}

	// surface beam
	{
		Com_Memset( &def, 0, sizeof(def) );
		def.state_bits = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
		def.face_culling = CT_FRONT_SIDED;
		def.primitives = TRIANGLE_STRIP;

		vk.std_pipeline.surface_beam_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	// axis for missing models
	{
		Com_Memset( &def, 0, sizeof(def) );
		def.state_bits = GLS_DEFAULT;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.face_culling = CT_TWO_SIDED;
		def.primitives = LINE_LIST;
		vk.std_pipeline.surface_axis_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	// debug pipelines.  ES 3.0 has no glPolygonMode, so GLS_POLYMODE_LINE is
	// realized by the LINE_LIST topology (as quake3e renderergles3 does).
	state_bits = GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE;

	{
		Com_Memset( &def, 0, sizeof(def) );
		def.state_bits = state_bits;
		def.shader_type = TYPE_COLOR_WHITE;
		def.face_culling = CT_FRONT_SIDED;
		def.primitives = LINE_LIST;
		vk.std_pipeline.tris_debug_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );

		def.face_culling = CT_BACK_SIDED;
		vk.std_pipeline.tris_mirror_debug_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );

		def.shader_type = TYPE_COLOR_GREEN;
		def.face_culling = CT_FRONT_SIDED;
		vk.std_pipeline.tris_debug_green_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );

		def.face_culling = CT_BACK_SIDED;
		vk.std_pipeline.tris_mirror_debug_green_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );

		def.shader_type = TYPE_COLOR_RED;
		def.face_culling = CT_FRONT_SIDED;
		vk.std_pipeline.tris_debug_red_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );

		def.face_culling = CT_BACK_SIDED;
		vk.std_pipeline.tris_mirror_debug_red_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	{
		Com_Memset( &def, 0, sizeof(def) );
		def.state_bits = GLS_DEPTHMASK_TRUE;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.primitives = LINE_LIST;
		vk.std_pipeline.normals_debug_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	{
		Com_Memset( &def, 0, sizeof(def) );
		def.state_bits = GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		vk.std_pipeline.surface_debug_pipeline_solid = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	{
		Com_Memset( &def, 0, sizeof(def) );
		def.state_bits = GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.primitives = LINE_LIST;
		vk.std_pipeline.surface_debug_pipeline_outline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	{
		Com_Memset( &def, 0, sizeof(def) );
		def.state_bits = GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.primitives = TRIANGLE_STRIP;
		vk.std_pipeline.images_debug_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}
}

void vk_create_pipelines( void )
{
	vk.pipelines_count = 0;
	vk.pipelines_world_base = 0;

	vk_alloc_persistent_pipelines();

	vk.pipelines_world_base = vk.pipelines_count;
}
