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

#include "tr_touchui_icons.h"

// Aurora touch-UI (port stage "Тач-UI (виртуальный геймпад)",
// gameport/docs/touch_ui.md): the cross-module contract between the client
// (shared/sdl/sdl_touchui.cpp - owns the layout, the per-finger hit-test and
// the action calls) and the renderer (code/rd-gles3/gles3_touchui.cpp -
// only draws what it is handed). refexport_t (tr_public.h) carries a
// function pointer that the client calls once a frame with a pointer to one
// of these; see the comment on Aurora_SetTouchOverlay there for why this is
// safe even though rd-vanilla/rd-vulkan never populate that pointer.
//
// Deliberately dependency-free (no q_shared.h, no engine types) so it can be
// included from the client, from tr_public.h and from the renderer's
// ImGui-facing translation unit alike - see tr_touchui_icons.h's comment for
// why that separation matters here specifically (GL_* name collisions).
//
// Coordinates are WINDOW pixels (matching vk.windowWidth/vk.windowHeight,
// i.e. exactly what SDL_GetWindowSize()/glViewport() already use for the
// final blit - see gles3_fbo_blit_to_screen()), NOT the game's logical/FBO
// "content" space (glConfig.vidWidth/vidHeight, which is rotated relative to
// the window at 90/270). The client already carries the one rotation
// transform this needs (Aurora_TouchUI_GetTransform(), sdl_touchui.h) and
// converts every point through it before filling this struct - see
// sdl_touchui.cpp's Aurora_TouchUI_ContentToWindow(). This way the renderer
// never has to know about vk.fbo.transform for touch-ui purposes at all: it
// just draws circles where it's told to, in the same native pixel space its
// own glViewport(0,0,vk.windowWidth,vk.windowHeight) already covers.
//
// Every element here is a circle (button or stick knob/base) on purpose:
// a circle's on-screen radius is rotation-invariant, so only its CENTER
// POINT needs the window<->content transform - no separate width/height
// swap logic for rectangles is needed anywhere in this feature.

#define AURORA_TOUCH_MAX_BUTTONS 11

typedef struct {
	float				x, y;			// window px, centre
	float				radius;			// window px - circle radius, used when label == NULL
	float				halfWidth;		// window px - capsule half-extents (VISUAL-local, i.e.
	float				halfHeight;		// pre-rotation - see gles3_touchui_draw.h), used when label != NULL
	auroraTouchIcon_t	icon;			// ignored by the renderer when label != NULL
	const char			*label;			// NULL: draw `icon` as a circle (procedural glyph), as
										// before. Non-NULL: draw a rounded-rect capsule sized
										// to halfWidth/halfHeight instead, this text centred in
										// it - a static string literal (e.g. "SAVE"/"LOAD"/
										// "SKIP"), so the pointer stays valid forever with no
										// lifetime tie to any per-frame buffer (port task B-003).
	int					pressed;		// 1 while a finger holds it (or it is latched)
} auroraTouchButton_t;

typedef struct {
	int		windowWidth, windowHeight;	// window size this frame was laid out for
	float	pixelsPerMm;				// for the renderer's stroke-thickness scaling
	float	alpha;						// 0..1 overlay opacity (cl_touchUIAlpha)
	int		transform;					// WL_OUTPUT_TRANSFORM_* (0/1/2/3) - see gles3_touchui_draw.h's comment: rotates icon shapes, positions are already window-space

	int					numButtons;
	auroraTouchButton_t	buttons[AURORA_TOUCH_MAX_BUTTONS];

	int		stick;						// 1 while the floating stick is shown
	float	stickBaseX, stickBaseY;		// window px, where the finger landed
	float	stickKnobX, stickKnobY;		// window px, clamped to stickRadius
	float	stickRadius;				// window px
} auroraTouchOverlay_t;
