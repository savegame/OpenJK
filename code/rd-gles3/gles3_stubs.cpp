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

// rd-gles3 (G1): no-op implementations of the compile-time VBO API.
//
// tr_local.h forces USE_VBO/USE_VBO_SS on (VBO_t/spriteStage_t are embedded
// unconditionally), but the actual static world/model VBO is a G5 item. These
// stubs keep the frontend linkable; since vboItemIndex stays 0 and
// shader->isStaticShader stays false, every runtime fast path in
// tr_surface/tr_backend/tr_world falls through to CPU tessellation.

#include "tr_local.h"

void R_BuildWorldVBO( msurface_t *surf, int surfCount )
{
	(void)surf;
	(void)surfCount;
	vk.vboWorldActive = qfalse;
}

void R_BuildSurfaceSpritesVBO( const world_t &worldData, int index )
{
	(void)worldData;
	(void)index;
	tr.ss.groups_count = 0;
}

void VBO_PushData( int itemIndex, shaderCommands_t *input )
{
	(void)itemIndex;
	(void)input;
}

void VBO_UnBind( void )
{
	tess.vbo_world_index = 0;
}

void VBO_Cleanup( void )
{
}

void VBO_QueueItem( int itemIndex )
{
	(void)itemIndex;
}

void VBO_ClearQueue( void )
{
}

void VBO_Flush( void )
{
}

void vk_clean_surface_sprites( void )
{
	tr.ss.groups_count = 0;
}

void vk_push_surface_sprites_cmd( const vk_ss_group_def_t *def, int firstInstance, int instanceCount )
{
	(void)def;
	(void)firstInstance;
	(void)instanceCount;
}

void RB_SurfaceSpritesVBO( srfSprites_t *surf )
{
	(void)surf;
}

#ifdef _G2_GORE
void R_CreateGoreVBO( void )
{
}

void R_UpdateGoreVBO( srfG2GoreSurface_t *goreSurface )
{
	(void)goreSurface;
}
#endif
