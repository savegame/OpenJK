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

// Aurora touch-UI (port stage "Тач-UI (виртуальный геймпад)",
// gameport/docs/touch_ui.md). This header has NO dependencies on purpose -
// it is shared between three worlds that must never see each other's
// headers in the same translation unit:
//   - the client (shared/sdl/sdl_touchui.cpp - picks an icon per button),
//   - the renderer's engine-facing side (code/rd-gles3/gles3_touchui.cpp -
//     includes tr_local.h/vk_local.h, which redefine GL_* compat names that
//     collide with the real GLES3 headers, see gles3_api.h's comment),
//   - the renderer's ImGui-facing side (code/rd-gles3/gles3_touchui_draw.cpp -
//     includes the real <GLES3/gl3.h> via imgui's OpenGL3 backend).
// Keeping this enum free of engine/GL includes lets all three include it
// safely.

typedef enum {
	AURORA_TOUCH_ICON_MENU,		// hamburger lines - always available
	AURORA_TOUCH_ICON_FIRE,		// crosshair - primary attack
	AURORA_TOUCH_ICON_ALTFIRE,		// crosshair with a cross - secondary/alt attack
	AURORA_TOUCH_ICON_JUMP,		// arrow up
	AURORA_TOUCH_ICON_CROUCH,		// arrow down onto a floor line
	AURORA_TOUCH_ICON_USE,			// concentric rings - interact/use
	AURORA_TOUCH_ICON_FORCE,		// radiating rays - use current force power
	AURORA_TOUCH_ICON_FORCE_PREV,	// chevron pointing left - forceprev
	AURORA_TOUCH_ICON_FORCE_NEXT,	// chevron pointing right - forcenext
	AURORA_TOUCH_ICON_ROLL,		// partial circular arc with an arrowhead -
									// tumble/roll (distinct from CROUCH's
									// straight arrow-onto-floor)

	AURORA_TOUCH_ICON_COUNT
} auroraTouchIcon_t;
