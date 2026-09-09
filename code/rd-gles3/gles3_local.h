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

// Internal helpers shared between the gles3_*.cpp backend units
// (not part of the vk_* frontend-facing API).

#pragma once

// viewport/scissor in GL coordinates (origin bottom-left)
void	gles3_get_viewport_rect( int *x, int *y, int *w, int *h );
void	gles3_get_scissor_rect( int *x, int *y, int *w, int *h );

// state cache: apply GLS_* state bits + cull mode + polygon offset of a
// pipeline def, delta-cached against the currently bound GL state
void	gles3_set_state( uint32_t state_bits, cullType_t face_culling, qboolean polygon_offset );
void	gles3_state_cache_invalidate( void );

// depth range -> viewport/depth range update (cached per command buffer)
void	gles3_update_depth_range( Vk_Depth_Range depth_range );

// geometry streaming: upload staged host data to the VBO/IBO
void	gles3_flush_geometry( void );
void	gles3_index_stage_destroy( void );

// uniform staging (gles3_geometry.cpp)
void	gles3_uniform_reset( void );
void	gles3_uniform_shutdown( void );

// upload vk.uniform + MVP to the active program (called at draw time)
void	gles3_apply_uniforms( void );
