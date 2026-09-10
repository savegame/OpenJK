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

// Debug logging + debug visualization for rd-gles3 (replaces vk_debug.cpp).

#include "tr_local.h"

void QDECL vk_debug( const char *msg, ... ) {
	va_list argptr;
	char text[1024];

	va_start( argptr, msg );
	Q_vsnprintf( text, sizeof(text), msg, argptr );
	va_end( argptr );

	ri.Printf( PRINT_ALL, "%s", text );
}

/*
================
DrawTris

Draws triangle outlines for debugging r_showtris
================
*/
void DrawTris( const shaderCommands_t *pInput )
{
	uint32_t pipeline;

	if ( tess.numIndexes == 0 )
		return;

	if ( r_fastsky->integer && pInput->shader->isSky )
		return;

#ifdef USE_PMLIGHT
	if ( tess.dlightPass )
		pipeline = backEnd.viewParms.portalView == PV_MIRROR ? vk.std_pipeline.tris_mirror_debug_red_pipeline : vk.std_pipeline.tris_debug_red_pipeline;
	else
#endif
		pipeline = ( backEnd.viewParms.portalView == PV_MIRROR ) ? vk.std_pipeline.tris_mirror_debug_pipeline : vk.std_pipeline.tris_debug_pipeline;

	// geometry and indexes are still bound from the stage iterator; the
	// tris pipeline draws the index stream as GL_LINES (LINE_LIST def)
	vk_bind_pipeline( pipeline );
	vk_draw_geometry( DEPTH_RANGE_ZERO, qtrue );
}

/*
================
DrawNormals

Draws vertex normals for debugging r_shownormals
================
*/
void DrawNormals( const shaderCommands_t *input )
{
	(void)input;
}

/*
===============
R_DebugGraphics

Visualization aid for shader debugging: r_debugSurface renders the surface
whose shader name matches the cvar.
===============
*/
void R_DebugGraphics( void )
{
}

void RB_ShowImages ( image_t** const pImg, uint32_t numImages )
{
	uint32_t	i;
	float		w, h, x, y;

	(void)pImg;
	(void)numImages;

	vk_set_2d();

	{
		const vec4_t black = { 0, 0, 0, 1 };
		vk_clear_color_attachments( black );
	}

	for (i = 0; i < tr.images.count; i++) {
		image_t *image = tr.images.items[i];

		w = glConfig.vidWidth / 20;
		h = glConfig.vidHeight / 15;
		x = i % 20 * w;
		y = i / 20 * h;

		// show in proportional size in mode 2
		if (r_showImages->integer == 2) {
			w *= image->uploadWidth / 512.0f;
			h *= image->uploadHeight / 512.0f;
		}

		vk_bind(image);

		Com_Memset(tess.svars.colors[0], 255, 4 * sizeof(color4ub_t));

		tess.numVertexes = 4;

		tess.xyz[0][0] = x;
		tess.xyz[0][1] = y;
		tess.svars.texcoords[0][0][0] = 0;
		tess.svars.texcoords[0][0][1] = 0;

		tess.xyz[1][0] = x + w;
		tess.xyz[1][1] = y;
		tess.svars.texcoords[0][1][0] = 1;
		tess.svars.texcoords[0][1][1] = 0;

		tess.xyz[2][0] = x;
		tess.xyz[2][1] = y + h;
		tess.svars.texcoords[0][2][0] = 0;
		tess.svars.texcoords[0][2][1] = 1;

		tess.xyz[3][0] = x + w;
		tess.xyz[3][1] = y + h;
		tess.svars.texcoords[0][3][0] = 1;
		tess.svars.texcoords[0][3][1] = 1;

		tess.svars.texcoordPtr[0] = tess.svars.texcoords[0];

		vk_bind_pipeline(vk.std_pipeline.images_debug_pipeline);
		vk_bind_geometry(TESS_XYZ | TESS_RGBA0 | TESS_ST0);
		vk_draw_geometry(DEPTH_RANGE_NORMAL, qfalse);
	}

	tess.numIndexes = 0;
	tess.numVertexes = 0;
}
