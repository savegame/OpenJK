/*
===========================================================================
Copyright (C) 2013 - 2018, OpenJK contributors

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

#pragma once

// Aurora touch-UI, engine-facing half (gles3_touchui.cpp). Safe to include
// from tr_local.h-including translation units (tr_init.cpp, gles3_frame.cpp)
// - unlike gles3_touchui_draw.h, this header pulls in nothing GL-related.

#include "../rd-common/tr_touchui.h"

// refexport_t entry point (tr_public.h) - stores a copy of *overlay (or
// clears the stored copy when overlay is NULL) for Aurora_TouchUI_Draw to
// pick up on the next gles3_fbo_blit_to_screen(). Called from the client
// (shared/sdl/sdl_touchui.cpp) once a frame.
void Aurora_TouchUI_SetOverlay( const auroraTouchOverlay_t *overlay );

// Called from vk_present_frame() (gles3_frame.cpp), right after
// gles3_fbo_blit_to_screen() and before ri.WIN_Present() - see
// research/touch_ui_research.md п.5 for why that exact spot needs no manual
// GL state restore.
void Aurora_TouchUI_Draw( void );

// Called from RE_Shutdown() while the GL context is still alive.
void Aurora_TouchUI_Shutdown( void );
