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

// Renderer info commands for rd-gles3 (replaces vk_info.cpp).

#include "tr_local.h"

const char *vk_shadertype_string( Vk_Shader_Type code )
{
	switch ( code ) {
		case TYPE_COLOR_BLACK: return "TYPE_COLOR_BLACK";
		case TYPE_COLOR_WHITE: return "TYPE_COLOR_WHITE";
		case TYPE_COLOR_GREEN: return "TYPE_COLOR_GREEN";
		case TYPE_COLOR_RED: return "TYPE_COLOR_RED";
		case TYPE_FOG_ONLY: return "TYPE_FOG_ONLY";
		case TYPE_DOT: return "TYPE_DOT";
		case TYPE_REFRACTION: return "TYPE_REFRACTION";
		case TYPE_SINGLE_TEXTURE_LIGHTING: return "TYPE_SINGLE_TEXTURE_LIGHTING";
		case TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR: return "TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR";
		case TYPE_SINGLE_TEXTURE_DF: return "TYPE_SINGLE_TEXTURE_DF";
		case TYPE_SINGLE_TEXTURE: return "TYPE_SINGLE_TEXTURE";
		default: return "TYPE_MULTI/BLEND";
	}
}

void vk_info_f( void )
{
	ri.Printf( PRINT_ALL, "\n" );
	ri.Printf( PRINT_ALL, "GL_VENDOR: %s\n", vk.vendor_string );
	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", vk.renderer_string );
	ri.Printf( PRINT_ALL, "GL_VERSION: %s\n", vk.version_string );
	ri.Printf( PRINT_ALL, "GL_EXTENSIONS: %s\n", vk.device_extensions_string );
	ri.Printf( PRINT_ALL, "\n" );
	ri.Printf( PRINT_ALL, "GL_MAX_TEXTURE_SIZE: %i\n", glConfig.maxTextureSize );
	ri.Printf( PRINT_ALL, "GL_MAX_TEXTURE_IMAGE_UNITS: %i\n", glConfig.maxActiveTextures );
	ri.Printf( PRINT_ALL, "\n" );
	ri.Printf( PRINT_ALL, "pipelines: %u\n", vk.pipelines_count );
}

void GfxInfo_f( void )
{
	vk_info_f();
}

void R_ShaderList_f( void ) {
	int				i;
	int				count;
	const shader_t	*sh;

	ri.Printf(PRINT_ALL, "-----------------------\n");

	count = 0;
	for (i = 0; i < tr.numShaders; i++) {
		if (ri.Cmd_Argc() > 1) {
			sh = tr.sortedShaders[i];
		}
		else {
			sh = tr.shaders[i];
		}

		ri.Printf( PRINT_ALL, "%i: ", sh->numUnfoggedPasses);
		ri.Printf( PRINT_ALL, "%s", sh->lightmapIndex[0] >= 0 ? "L " : "  ");
		ri.Printf( PRINT_ALL, "%s", sh->multitextureEnv ? "MT(x) " : "  ");
		ri.Printf( PRINT_ALL, "%s", sh->explicitlyDefined ? "E " : "  ");
		ri.Printf( PRINT_ALL, "%s", sh->sky ? "sky" : "gen");
		ri.Printf( PRINT_ALL, ": %s %s\n", sh->name, sh->defaultShader ? "(DEFAULTED)" : "");

		count++;
	}
	ri.Printf( PRINT_ALL, "%i total shaders\n", count);
	ri.Printf( PRINT_ALL, "------------------\n");
}
