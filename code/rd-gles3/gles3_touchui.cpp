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

// Aurora touch-UI - engine-facing half (port stage "Тач-UI (виртуальный
// геймпад)", gameport/docs/touch_ui.md; research in
// research/touch_ui_research.md).
//
// This file is the ONLY bridge between the engine world (tr_local.h/
// vk_local.h, included below) and the ImGui-drawing world
// (gles3_touchui_draw.h/.cpp, which must never see tr_local.h - see the
// comment on gles3_touchui_draw.h for the GL_* name collision this avoids).
// It does no drawing itself: it just stores the client's latest overlay
// (received via refexport_t::Aurora_SetTouchOverlay, see tr_public.h) and,
// once a frame, repackages it into the plain struct gles3_touchui_draw.cpp
// understands, with zero GL calls of its own.
//
// Coordinates are already WINDOW pixels by the time they reach here (see
// tr_touchui.h's comment) - the client (shared/sdl/sdl_touchui.cpp) does the
// window<->content rotation transform itself, so this file has nothing to
// convert, only to sanity-check (overlay laid out for a stale window size
// is dropped for one frame rather than drawn stretched/misplaced).

#include "tr_local.h"
#include "gles3_touchui.h"
#include "gles3_touchui_draw.h"

static auroraTouchOverlay_t aurora_touchOverlay;
static qboolean aurora_touchOverlayValid = qfalse;

void Aurora_TouchUI_SetOverlay( const auroraTouchOverlay_t *overlay )
{
	if ( !overlay || ( overlay->numButtons <= 0 && !overlay->stick ) )
	{
		aurora_touchOverlayValid = qfalse;
		return;
	}

	aurora_touchOverlay = *overlay;
	aurora_touchOverlayValid = qtrue;
}

void Aurora_TouchUI_Draw( void )
{
	AuroraTouchDrawFrame frame;
	int i;

	if ( !aurora_touchOverlayValid )
	{
		return;
	}

	// Laid out for a window size we no longer have (rotation/resize raced
	// the client's next Aurora_TouchUI_Frame() call) - skip this one frame
	// rather than draw stretched/misplaced circles; the client re-lays-out
	// and re-sends every frame regardless, so this self-heals immediately.
	if ( aurora_touchOverlay.windowWidth != (int)vk.windowWidth ||
		aurora_touchOverlay.windowHeight != (int)vk.windowHeight )
	{
		return;
	}

	Com_Memset( &frame, 0, sizeof( frame ) );
	frame.windowWidth  = aurora_touchOverlay.windowWidth;
	frame.windowHeight = aurora_touchOverlay.windowHeight;
	frame.alpha        = aurora_touchOverlay.alpha;
	frame.pixelsPerMm  = aurora_touchOverlay.pixelsPerMm;
	frame.transform    = aurora_touchOverlay.transform;

	// The draw-side array must hold every button the client can send, or the
	// last ones are silently dropped here while still being hit-tested on the
	// client side (TB_WEAP_NEXT was invisible but tappable exactly this way).
	static_assert( AURORA_TOUCH_DRAW_MAX_BUTTONS >= AURORA_TOUCH_MAX_BUTTONS,
		"AURORA_TOUCH_DRAW_MAX_BUTTONS must be >= AURORA_TOUCH_MAX_BUTTONS" );

	frame.numButtons = aurora_touchOverlay.numButtons;
	if ( frame.numButtons > AURORA_TOUCH_DRAW_MAX_BUTTONS )
	{
		frame.numButtons = AURORA_TOUCH_DRAW_MAX_BUTTONS;
	}
	for ( i = 0; i < frame.numButtons; i++ )
	{
		frame.buttons[i].x          = aurora_touchOverlay.buttons[i].x;
		frame.buttons[i].y          = aurora_touchOverlay.buttons[i].y;
		frame.buttons[i].radius     = aurora_touchOverlay.buttons[i].radius;
		frame.buttons[i].halfWidth  = aurora_touchOverlay.buttons[i].halfWidth;
		frame.buttons[i].halfHeight = aurora_touchOverlay.buttons[i].halfHeight;
		frame.buttons[i].icon       = aurora_touchOverlay.buttons[i].icon;
		frame.buttons[i].label      = aurora_touchOverlay.buttons[i].label;
		frame.buttons[i].pressed    = aurora_touchOverlay.buttons[i].pressed;
		frame.buttons[i].disabled   = aurora_touchOverlay.buttons[i].disabled;
	}

	frame.stick       = aurora_touchOverlay.stick;
	frame.stickBaseX  = aurora_touchOverlay.stickBaseX;
	frame.stickBaseY  = aurora_touchOverlay.stickBaseY;
	frame.stickKnobX  = aurora_touchOverlay.stickKnobX;
	frame.stickKnobY  = aurora_touchOverlay.stickKnobY;
	frame.stickRadius = aurora_touchOverlay.stickRadius;

	Aurora_TouchUI_RenderFrame( &frame );
}

void Aurora_TouchUI_Shutdown( void )
{
	Aurora_TouchUI_ShutdownDraw();
	aurora_touchOverlayValid = qfalse;
}
