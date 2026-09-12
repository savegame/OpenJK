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

// Aurora touch-UI, ImGui-facing half (gles3_touchui_draw.cpp).
//
// This header (and its .cpp) must NEVER include tr_local.h/vk_local.h or
// anything that drags them in: those headers define their own GL_* compat
// enum (gles3_api.h's comment - "the real GL preprocessor constants...
// collide") that gles3_api.h keeps out of scope specifically so it never
// meets a real <GLES3/gl3.h>. imgui's OpenGL3 backend (lib/imgui/backends/
// imgui_impl_opengl3.cpp) DOES include the real <GLES3/gl3.h> (built with
// IMGUI_IMPL_OPENGL_ES3, see code/rd-gles3/CMakeLists.txt) - so this
// translation unit and any tr_local.h-including one must stay strictly
// separate. gles3_touchui.cpp is the only bridge between them, and it only
// ever passes the plain-old-data below across.
//
// Coordinates here are WINDOW pixels already (see tr_touchui.h's comment) -
// this file does no rotation math of its own, only draws circles where it
// is told to.

#include "../rd-common/tr_touchui_icons.h"

#define AURORA_TOUCH_DRAW_MAX_BUTTONS 8

typedef struct {
	float				x, y, radius;	// window px
	auroraTouchIcon_t	icon;
	int					pressed;
} AuroraTouchButtonDraw;

typedef struct {
	int		windowWidth, windowHeight;
	float	alpha;
	float	pixelsPerMm;

	int						numButtons;
	AuroraTouchButtonDraw	buttons[AURORA_TOUCH_DRAW_MAX_BUTTONS];

	int		stick;
	float	stickBaseX, stickBaseY;
	float	stickKnobX, stickKnobY;
	float	stickRadius;
} AuroraTouchDrawFrame;

// Draws *frame with a private ImGui context (created lazily on first use)
// straight into whichever framebuffer is currently bound - the caller
// (gles3_touchui.cpp) is responsible for that being the real window
// framebuffer, viewport already covering it. Only ImGui::GetForegroundDrawList()
// primitives are used (no windows, no widgets, no input fed to this
// context) - hit-testing is entirely the client's job (shared/sdl/
// sdl_touchui.cpp), this is rendering only.
void Aurora_TouchUI_RenderFrame( const AuroraTouchDrawFrame *frame );

// Destroys the private ImGui context and its GL objects, if created.
void Aurora_TouchUI_ShutdownDraw( void );
