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

// Lens flares for rd-gles3 (replaces vk_flares.cpp).
//
// G1: the Vulkan version used a storage-buffer dot pipeline for occlusion
// tests; port to GL_ANY_SAMPLES_PASSED occlusion queries is scheduled for
// G4 together with the other effects.  Flares are inert until then.

#include "tr_local.h"

void R_ClearFlares( void )
{
}

void RB_AddFlare( void *surface, int fogNum, vec3_t point, vec3_t color, vec3_t normal )
{
	(void)surface; (void)fogNum; (void)point; (void)color; (void)normal;
}

void RB_RenderFlares( void )
{
}
