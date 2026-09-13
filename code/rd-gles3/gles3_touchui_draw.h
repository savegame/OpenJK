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
// button/stick CENTRES are already transformed into window space by the
// client, so this file does no positional rotation math of its own. Icon
// SHAPES are a different matter: an icon is drawn as a handful of offsets
// from its own centre (e.g. an arrow's "up" direction), and those offsets
// are defined in the same visual/landscape space the client lays the
// button out in - NOT window space, which is rotated relative to it
// whenever `transform` != 0 (see research/touch_ui_research.md п.8, and
// sdl_touchui.cpp's Aurora_TouchUI_WindowToVisual/VisualToWindow for the
// position side of the same rotation). `transform` below is that same
// WL_OUTPUT_TRANSFORM_* value (0/1/2/3), passed through unchanged so this
// file can rotate each icon's local offsets the same way before drawing -
// see Aurora_TouchUI_RotateOffset in the .cpp.

#include "../rd-common/tr_touchui_icons.h"

#define AURORA_TOUCH_DRAW_MAX_BUTTONS 12

typedef struct {
	float				x, y, radius;	// window px - radius used when label == NULL (icon circle)
	float				halfWidth;		// window px - capsule half-extents (VISUAL-local, i.e.
	float				halfHeight;		// pre-rotation - the renderer rotates the quad itself,
										// see Aurora_TouchUI_AddCapsule), used when label != NULL
	auroraTouchIcon_t	icon;			// ignored when label != NULL
	const char			*label;			// NULL: procedural icon glyph on a circle (as before).
										// Non-NULL: a rounded-rect capsule sized to halfWidth/
										// halfHeight, this text centred in it (SAVE/LOAD/SKIP,
										// port task B-003) - see Aurora_TouchUI_AddCapsule in
										// the .cpp for how both the capsule shape and the text
										// inside it get the same per-orientation rotation
										// treatment icon shapes already use (rotating the
										// already-laid-out quad/glyph vertices about the button
										// centre, not the shapes' own local definitions).
	int					pressed;
} AuroraTouchButtonDraw;

typedef struct {
	int		windowWidth, windowHeight;
	float	alpha;
	float	pixelsPerMm;
	int		transform;	// WL_OUTPUT_TRANSFORM_* (0/1/2/3) - rotates icon shapes only, positions are already in window space

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
