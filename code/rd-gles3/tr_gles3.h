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

#pragma once

// rd-gles3 (AuroraOS): GLES3 shader/pipeline framework, GL state cache and
// geometry streaming. Modelled after the gles3 renderer in quake3e
// (code/renderergles3/gles3.c), adapted to C++ and to the JA rd-vanilla
// backend conventions (standard GL depth: LEQUAL, clear depth 1.0).

typedef enum {
	G3_PROG_TEXTURE,	// single texture modulated by vertex color
	G3_PROG_COLOR,		// vertex color only (white, no texture fetch)
	G3_PROG_COUNT
} g3_program_type_t;

typedef struct {
	g3_program_type_t type;
	uint32_t		state_bits;		// GLS_* mask (blend/depth/atest)
	cullType_t		cull_type;		// CT_FRONT_SIDED / CT_BACK_SIDED / CT_TWO_SIDED
	bool			mirror;			// flip cull face like vanilla GL_Cull did
	bool			polygon_offset;
} g3_pipeline_def_t;

void	g3_init( void );		// (re)initialize state cache, call after GL_SetDefaultState

// GL_State / GL_Cull replacements for the rd-vanilla backend entry points:
// same semantics, but the actual GL calls go through the state cache above.
void	g3_state_bits( uint32_t state_bits );
void	g3_cull( cullType_t cull_type, bool mirror );

// Compile (or fetch from cache) the GL program for a pipeline definition.
GLuint	g3_get_program( const g3_pipeline_def_t *def );

// Full tess-batch draw: uploads xyz/color/texcoord attributes of the current
// tess, applies pipeline state, MVP and issues glDrawElements.
void	g3_draw_tess( const g3_pipeline_def_t *def, int numIndexes, const glIndex_t *indexes );

// Low-level immediate draw from caller-owned arrays (2D quads: splash, ROQ,
// dissolve blits, image browser). Arrays are consumed before returning.
void	g3_draw_arrays( const g3_pipeline_def_t *def, GLenum mode, int numVerts,
					const float *xyz4, const byte *color4ub, const float *texcoord2 );

// Current MVP (2D ortho or 3D projection*modelview), column-major 4x4.
void	g3_get_mvp( float *mvp16 );

// called by RB_SwapBuffers after WIN_Present; the next draw of the new
// frame orphans the geometry storage again
void	g3_frame_end( void );
