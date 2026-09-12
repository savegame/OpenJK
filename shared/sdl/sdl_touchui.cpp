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

// Aurora touch-UI - gameplay virtual gamepad (port stage "Тач-UI
// (виртуальный геймпад)", gameport/docs/touch_ui.md; research in
// research/touch_ui_research.md). Layout/hit-test/action-dispatch only -
// drawing is the renderer's job (code/rd-gles3/gles3_touchui*.cpp), reached
// once a frame via refexport_t::Aurora_SetTouchOverlay (tr_public.h).
//
// Layout reference (user-provided, port task B-002): the on-screen gamepad
// of ~/Projects/aurora-ports/quake4/quake4es (sys/sdl/touch_ui.cpp) - read
// there for the general shape (floating left stick, right camera pad, a
// large primary-action button anchored bottom-right flanked by smaller
// utility buttons, menu button top-left, edges kept clear for Aurora's
// system gestures) and adapted to JKA's much smaller action set (6 buttons
// here vs. that game's 12) rather than ported pixel-for-pixel.
//
// Design points this file exists to satisfy (gameport/docs/touch_ui.md,
// work/backlog.md B-002):
//   - floating stick (left half): centre lands where the finger touched,
//     normalized vector with a dead zone;
//   - camera pad (right half): emulates the mouse, through the SAME
//     rotation transform real mouse motion already goes through
//     (Aurora_TransformInputDeltaF's derivation in sdl_input.cpp - this
//     file keeps an independent copy of that tiny table, see
//     Aurora_TouchUI_RotateVector below, since sdl_input.cpp's copy is
//     `static`);
//   - action buttons call the engine's real +action/-action commands via
//     Cbuf_ExecuteText (see research doc п.2 for why this, not key
//     emulation, is the correct call);
//   - multitouch: stick, pad and every button own an SDL_FingerID and only
//     react to their own finger until it lifts;
//   - millimetre sizing from physical DPI;
//   - gameplay-only visibility, physical-input override, edge margins.

#include <SDL.h>
#include <math.h>

#include "qcommon/qcommon.h"
#include "qcommon/q_shared.h"
#include "client/client.h"
#include "sys/sys_local.h"
#include "sdl_touchui.h"

#ifdef AURORA

extern refexport_t re;

// ---------------------------------------------------------------------------
// cvars
// ---------------------------------------------------------------------------

static cvar_t *cl_touchUI;				// 0 off, 1 auto (touch seen, no physical input), 2 always on
static cvar_t *cl_touchUIAlpha;			// 0.1..1.0 overlay opacity
static cvar_t *cl_touchUIButtonSize;		// mm, action button diameter
static cvar_t *cl_touchUIEdgeMargin;		// mm, kept clear of Aurora's edge gestures
static cvar_t *cl_touchUIStickRadius;		// mm, stick travel to reach full deflection
static cvar_t *cl_touchUIStickDeadZone;	// mm
static cvar_t *cl_touchUILookSpeed;		// mouse units per mm the camera-pad finger travels

// ---------------------------------------------------------------------------
// action buttons
// ---------------------------------------------------------------------------

typedef enum {
	TB_MENU,
	TB_FIRE,
	TB_ALTFIRE,
	TB_JUMP,
	TB_CROUCH,
	TB_USE,
	TB_COUNT
} touchButtonIndex_t;

typedef struct {
	const char			*downCmd;	// "+attack\n" etc - NULL for the menu button (sends A_ESCAPE instead)
	const char			*upCmd;		// "-attack\n"
	auroraTouchIcon_t	icon;
	int					latch;		// 1: a tap toggles it on, the next tap off (crouch)

	// runtime state
	int			held;
	SDL_FingerID	finger;
	int			down;		// action currently applied (for non-latch buttons)
	int			latched;	// latch currently on (for latch buttons)

	// layout, VISUAL mm (see Aurora_TouchUI_Layout) - centre + radius
	float		cx, cy, radiusMm;
} touchButton_t;

static touchButton_t touchButtons[TB_COUNT] = {
	{ NULL,           NULL,            AURORA_TOUCH_ICON_MENU,    0 },
	{ "+attack\n",    "-attack\n",     AURORA_TOUCH_ICON_FIRE,    0 },
	{ "+altattack\n", "-altattack\n",  AURORA_TOUCH_ICON_ALTFIRE, 0 },
	{ "+moveup\n",    "-moveup\n",     AURORA_TOUCH_ICON_JUMP,    0 },
	{ "+movedown\n",  "-movedown\n",   AURORA_TOUCH_ICON_CROUCH,  1 },
	{ "+use\n",       "-use\n",        AURORA_TOUCH_ICON_USE,     0 },
};

// ---------------------------------------------------------------------------
// stick (left half) and camera pad (right half)
// ---------------------------------------------------------------------------

static struct {
	int				held;
	SDL_FingerID	finger;
	float			baseX, baseY;	// VISUAL mm, where the finger landed
	float			curX, curY;		// VISUAL mm, current finger position (clamped to radius for the knob)
	float			side, forward;	// -1..1, movement axes
} touchStick;

static struct {
	int				held;
	SDL_FingerID	finger;
	float			lastX, lastY;	// WINDOW px (native, matches SDL_MOUSEMOTION's own space)
	float			remX, remY;		// FBO-local px, sub-pixel carry - mirrors sdl_input.cpp's aurora_touch.remX/Y
} touchPad;

// ---------------------------------------------------------------------------
// visibility state
// ---------------------------------------------------------------------------

static int touchScreenSeen = 0;		// a real SDL_FINGERDOWN has happened at least once
static int touchPhysicalOverride = 0;	// a real mouse/key/gamepad event since the last touch
static int touchJoystickConnected = 0;

static int touchLayoutWindowW = 0;
static int touchLayoutWindowH = 0;
static float touchPixelsPerMm = 96.0f / 25.4f;

/*
=================
Aurora_TouchUI_PixelsPerMm

Diagonal DPI first (steadier on drivers that swap width/height reporting),
falling back to vertical DPI, then a sane 96dpi default - same order as the
already-verified reference quake4es (touch_ui.cpp, TouchUI_PixelsPerMm) and
the existing DPI code in gameport/examples/launcher/launcher.cpp.
=================
*/
static float Aurora_TouchUI_PixelsPerMm( void )
{
	SDL_Window *window = Aurora_TouchUI_GetWindow();
	float ddpi = 0.0f, hdpi = 0.0f, vdpi = 0.0f, dpi = 0.0f;
	int display = window ? SDL_GetWindowDisplayIndex( window ) : -1;

	if ( SDL_GetDisplayDPI( display >= 0 ? display : 0, &ddpi, &hdpi, &vdpi ) == 0 )
	{
		dpi = ( ddpi > 1.0f ) ? ddpi : vdpi;
	}
	if ( dpi <= 1.0f )
	{
		dpi = 96.0f;
	}

	dpi = dpi / 25.4f;
	if ( dpi < 2.0f ) dpi = 2.0f;
	if ( dpi > 40.0f ) dpi = 40.0f;
	return dpi;
}

/*
=================
Aurora_TouchUI_WindowToVisual / VisualToWindow

The window is a fixed physical rectangle (native panel shape, e.g. a
portrait 1080x2400 - see research doc п.8) that never resizes on a pure
rotation; the "visual" space this module lays the gamepad out in is always
the LANDSCAPE orientation the player currently sees (so "left half" and
"right half" mean what they visually look like, at any of the 4 physical
holds). visualW/visualH are simply window's two dimensions relabelled
landscape (max/min) - the FBO the renderer blits is that same relabelling
(glConfig.vidWidth/Height, "always the exact transpose of the window",
gles3_fbo.cpp) so this needs no renderer-side lookup at all.

The rotation table is the exact one already derived and verified for real
mouse/menu-trackpad deltas (Aurora_TransformInputDeltaF's big comment in
sdl_input.cpp) - kept as an independent copy here because that function is
`static` in a different translation unit; window<->visual for an ABSOLUTE
point additionally centres/re-offsets around each rectangle's own centre
(a vector's rotation needs no such translation, which is why the delta-only
version never needed it).
=================
*/
static void Aurora_TouchUI_VisualSize( int windowW, int windowH, int *visualW, int *visualH )
{
	if ( windowW >= windowH )
	{
		*visualW = windowW;
		*visualH = windowH;
	}
	else
	{
		*visualW = windowH;
		*visualH = windowW;
	}
}

static void Aurora_TouchUI_WindowToVisual( float wx, float wy, int windowW, int windowH, float *vx, float *vy )
{
	const int t = Aurora_TouchUI_GetTransform();
	int visualW, visualH;
	float wx2, wy2, cx2, cy2;

	Aurora_TouchUI_VisualSize( windowW, windowH, &visualW, &visualH );

	wx2 = wx - (float)windowW * 0.5f;
	wy2 = wy - (float)windowH * 0.5f;

	switch ( t )
	{
		default:
		case 0: cx2 =  wx2; cy2 =  wy2; break;	// WL_OUTPUT_TRANSFORM_NORMAL
		case 1: cx2 = -wy2; cy2 =  wx2; break;	// 90
		case 2: cx2 = -wx2; cy2 = -wy2; break;	// 180
		case 3: cx2 =  wy2; cy2 = -wx2; break;	// 270
	}

	*vx = cx2 + (float)visualW * 0.5f;
	*vy = cy2 + (float)visualH * 0.5f;
}

static void Aurora_TouchUI_VisualToWindow( float vx, float vy, int windowW, int windowH, float *wx, float *wy )
{
	const int t = Aurora_TouchUI_GetTransform();
	int visualW, visualH;
	float vx2, vy2, wx2, wy2;

	Aurora_TouchUI_VisualSize( windowW, windowH, &visualW, &visualH );

	vx2 = vx - (float)visualW * 0.5f;
	vy2 = vy - (float)visualH * 0.5f;

	// Inverse of the table above (solved algebraically - see
	// research/touch_ui_research.md п.8 for the derivation).
	switch ( t )
	{
		default:
		case 0: wx2 =  vx2; wy2 =  vy2; break;
		case 1: wx2 =  vy2; wy2 = -vx2; break;
		case 2: wx2 = -vx2; wy2 = -vy2; break;
		case 3: wx2 = -vy2; wy2 =  vx2; break;
	}

	*wx = wx2 + (float)windowW * 0.5f;
	*wy = wy2 + (float)windowH * 0.5f;
}

/*
=================
Aurora_TouchUI_Layout

Positions every button and the stick/pad zones in VISUAL millimetres,
landscape, regardless of which of the 4 physical orientations the window is
currently held in. Re-run whenever the window size or the relevant cvars
change (cheap; guarded so it does not redo the float math every frame for
nothing).
=================
*/
static void Aurora_TouchUI_Layout( void )
{
	SDL_Window *window = Aurora_TouchUI_GetWindow();
	int windowW = 0, windowH = 0;
	int visualW, visualH;
	float margin, sizeMm, smallMm, bigMm, gapMm;
	float visualWmm, visualHmm, right, bottom;
	static float lastButtonSize = -1.0f, lastEdgeMargin = -1.0f;

	if ( window )
	{
		SDL_GetWindowSize( window, &windowW, &windowH );
	}
	if ( windowW <= 0 ) windowW = 1;
	if ( windowH <= 0 ) windowH = 1;

	if ( windowW == touchLayoutWindowW && windowH == touchLayoutWindowH &&
		cl_touchUIButtonSize->value == lastButtonSize &&
		cl_touchUIEdgeMargin->value == lastEdgeMargin )
	{
		return;
	}

	touchLayoutWindowW = windowW;
	touchLayoutWindowH = windowH;
	lastButtonSize = cl_touchUIButtonSize->value;
	lastEdgeMargin = cl_touchUIEdgeMargin->value;
	touchPixelsPerMm = Aurora_TouchUI_PixelsPerMm();

	Aurora_TouchUI_VisualSize( windowW, windowH, &visualW, &visualH );
	visualWmm = (float)visualW / touchPixelsPerMm;
	visualHmm = (float)visualH / touchPixelsPerMm;

	margin  = cl_touchUIEdgeMargin->value;
	sizeMm  = cl_touchUIButtonSize->value;
	smallMm = sizeMm * 0.85f;
	bigMm   = sizeMm * 1.5f;
	gapMm   = sizeMm * 0.35f;

	right  = visualWmm - margin;
	bottom = visualHmm - margin;

	// Top-left: the menu button, always alone up there.
	touchButtons[TB_MENU].radiusMm = smallMm * 0.5f;
	touchButtons[TB_MENU].cx = margin + smallMm * 0.5f;
	touchButtons[TB_MENU].cy = margin + smallMm * 0.5f;

	// Bottom-right cluster: fire is the large central button (the thumb's
	// resting position), jump above it, crouch to its left, alt-fire on the
	// upper-left diagonal, use further out to the left - mirrors the
	// reference's "large primary action, flanked by smaller utility
	// buttons, thumb never has to leave the corner" shape.
	touchButtons[TB_FIRE].radiusMm = bigMm * 0.5f;
	touchButtons[TB_FIRE].cx = right - bigMm * 0.5f;
	touchButtons[TB_FIRE].cy = bottom - bigMm * 0.5f;

	touchButtons[TB_JUMP].radiusMm = smallMm * 0.5f;
	touchButtons[TB_JUMP].cx = touchButtons[TB_FIRE].cx;
	touchButtons[TB_JUMP].cy = touchButtons[TB_FIRE].cy - bigMm * 0.5f - gapMm - smallMm * 0.5f;

	touchButtons[TB_CROUCH].radiusMm = smallMm * 0.5f;
	touchButtons[TB_CROUCH].cx = touchButtons[TB_FIRE].cx - bigMm * 0.5f - gapMm - smallMm * 0.5f;
	touchButtons[TB_CROUCH].cy = touchButtons[TB_FIRE].cy;

	touchButtons[TB_ALTFIRE].radiusMm = smallMm * 0.5f;
	touchButtons[TB_ALTFIRE].cx = touchButtons[TB_CROUCH].cx;
	touchButtons[TB_ALTFIRE].cy = touchButtons[TB_JUMP].cy;

	touchButtons[TB_USE].radiusMm = smallMm * 0.5f;
	touchButtons[TB_USE].cx = touchButtons[TB_CROUCH].cx - smallMm - gapMm;
	touchButtons[TB_USE].cy = touchButtons[TB_FIRE].cy;
}

/*
=================
Aurora_TouchUI_Enabled

Off (0): never. Always (2): always shown, physical input or not - mainly
for testing on a host with no touchscreen at all. Auto (1, default):
whenever the touchscreen is the thing being used - hidden the moment a
physical mouse/keyboard/gamepad shows up, back the next time the screen is
touched (gameport/docs/touch_ui.md, "Физический ввод вытесняет тач-UI").
=================
*/
static int Aurora_TouchUI_Enabled( void )
{
	switch ( cl_touchUI->integer )
	{
		case 0: return 0;
		case 2: return 1;
	}
	return touchScreenSeen && !touchPhysicalOverride && !touchJoystickConnected;
}

static int Aurora_TouchUI_InGameplay( void )
{
	return cls.state == CA_ACTIVE && !( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) );
}

/*
=================
Aurora_TouchUI_Action
=================
*/
static void Aurora_TouchUI_ReleaseButton( touchButton_t *b )
{
	if ( !b->held && !b->down && !b->latched )
	{
		return;
	}

	b->held = 0;

	if ( !b->downCmd )
	{
		// the menu button - nothing latches, FingerUp already sent the key
		return;
	}

	if ( !b->latch && b->down )
	{
		Cbuf_ExecuteText( EXEC_APPEND, b->upCmd );
		b->down = 0;
	}
}

static void Aurora_TouchUI_PressButton( touchButton_t *b, SDL_FingerID finger )
{
	b->held = 1;
	b->finger = finger;

	if ( !b->downCmd )
	{
		Sys_QueEvent( 0, SE_KEY, A_ESCAPE, qtrue, 0, NULL );
		Sys_QueEvent( 0, SE_KEY, A_ESCAPE, qfalse, 0, NULL );
		return;
	}

	if ( b->latch )
	{
		if ( b->latched )
		{
			Cbuf_ExecuteText( EXEC_APPEND, b->upCmd );
			b->latched = 0;
		}
		else
		{
			Cbuf_ExecuteText( EXEC_APPEND, b->downCmd );
			b->latched = 1;
		}
		return;
	}

	if ( !b->down )
	{
		Cbuf_ExecuteText( EXEC_APPEND, b->downCmd );
		b->down = 1;
	}
}

static void Aurora_TouchUI_StickStop( void )
{
	touchStick.held = 0;
	touchStick.side = touchStick.forward = 0.0f;
}

static void Aurora_TouchUI_StickMove( float vx, float vy )
{
	const float radiusMm = cl_touchUIStickRadius->value;
	const float deadMm = ( cl_touchUIStickDeadZone->value < radiusMm * 0.5f ) ? cl_touchUIStickDeadZone->value : radiusMm * 0.5f;
	float dx = vx - touchStick.baseX;
	float dy = vy - touchStick.baseY;
	float len = sqrtf( dx * dx + dy * dy );

	if ( len > radiusMm )
	{
		dx *= radiusMm / len;
		dy *= radiusMm / len;
		len = radiusMm;
	}
	touchStick.curX = touchStick.baseX + dx;
	touchStick.curY = touchStick.baseY + dy;

	if ( len <= deadMm )
	{
		touchStick.side = touchStick.forward = 0.0f;
		return;
	}

	{
		const float scale = ( len - deadMm ) / ( radiusMm - deadMm ) / len;
		touchStick.side = dx * scale;
		touchStick.forward = -dy * scale;	// screen-down is negative forward
	}
}

// Discretizes the analog stick into the engine's real movement actions
// (+forward/+back/+moveleft/+moveright - never emulated keys, see the file
// comment) - up to 8-directional, edge-triggered so each command is sent
// exactly once per press/release rather than every frame.
static void Aurora_TouchUI_StickApply( void )
{
	static int wasForward = 0, wasBack = 0, wasLeft = 0, wasRight = 0;
	const float threshold = 0.35f;
	int forward = touchStick.held && touchStick.forward >  threshold;
	int back    = touchStick.held && touchStick.forward < -threshold;
	int left    = touchStick.held && touchStick.side    < -threshold;
	int right   = touchStick.held && touchStick.side    >  threshold;

	if ( forward != wasForward ) Cbuf_ExecuteText( EXEC_APPEND, forward ? "+forward\n" : "-forward\n" );
	if ( back    != wasBack    ) Cbuf_ExecuteText( EXEC_APPEND, back    ? "+back\n"    : "-back\n" );
	if ( left    != wasLeft    ) Cbuf_ExecuteText( EXEC_APPEND, left    ? "+moveleft\n"  : "-moveleft\n" );
	if ( right   != wasRight   ) Cbuf_ExecuteText( EXEC_APPEND, right   ? "+moveright\n" : "-moveright\n" );

	wasForward = forward;
	wasBack = back;
	wasLeft = left;
	wasRight = right;
}

/*
=================
Aurora_TouchUI_PadMove

Camera pad: synthesizes the exact same SE_MOUSE deltas a real
SDL_MOUSEMOTION would produce, through the identical rotation/scale
transform (Aurora_TouchUI_RotateDelta mirrors sdl_input.cpp's
Aurora_TransformInputDeltaF - see that function's derivation comment; the
scale factor (cl_auroraFboScale) is re-read here rather than duplicated,
since it is already the cross-module bridge cvar gles3_fbo_set_scale
maintains).
=================
*/
static void Aurora_TouchUI_RotateDelta( float dx, float dy, float *outx, float *outy )
{
	const int t = Aurora_TouchUI_GetTransform();
	float scale = Cvar_VariableValue( "cl_auroraFboScale" );
	float fx, fy;

	if ( scale <= 0.0f ) scale = 1.0f;
	fx = dx / scale;
	fy = dy / scale;

	switch ( t )
	{
		default:
		case 0: *outx =  fx; *outy =  fy; break;
		case 1: *outx = -fy; *outy =  fx; break;
		case 2: *outx = -fx; *outy = -fy; break;
		case 3: *outx =  fy; *outy = -fx; break;
	}
}

static void Aurora_TouchUI_PadMove( float windowX, float windowY )
{
	const float mmPerPx = 1.0f / touchPixelsPerMm;
	const float unitsPerMm = cl_touchUILookSpeed->value;
	float dxWin = windowX - touchPad.lastX;
	float dyWin = windowY - touchPad.lastY;
	float dxUnits, dyUnits, tdx, tdy;
	int idx, idy;

	touchPad.lastX = windowX;
	touchPad.lastY = windowY;

	dxUnits = dxWin * mmPerPx * unitsPerMm;
	dyUnits = dyWin * mmPerPx * unitsPerMm;

	Aurora_TouchUI_RotateDelta( dxUnits, dyUnits, &tdx, &tdy );

	touchPad.remX += tdx;
	touchPad.remY += tdy;
	idx = (int)touchPad.remX;
	idy = (int)touchPad.remY;
	touchPad.remX -= (float)idx;
	touchPad.remY -= (float)idy;

	if ( idx || idy )
	{
		Sys_QueEvent( 0, SE_MOUSE, idx, idy, 0, NULL );
	}
}

/*
=================
Aurora_TouchUI_FingerEvent
=================
*/
void Aurora_TouchUI_FingerEvent( const SDL_Event *ev )
{
	const SDL_TouchFingerEvent &finger = ev->tfinger;
	int windowW = touchLayoutWindowW, windowH = touchLayoutWindowH;
	float vx, vy;
	int i;

	Aurora_TouchUI_Layout();
	windowW = touchLayoutWindowW;
	windowH = touchLayoutWindowH;
	if ( windowW <= 0 || windowH <= 0 )
	{
		return;
	}

	if ( ev->type == SDL_FINGERDOWN )
	{
		touchScreenSeen = 1;
		touchPhysicalOverride = 0;
	}

	// SDL_TouchFingerEvent's x/y are normalized [0,1] over the WINDOW
	// (native axes) - convert to window px, then to visual (landscape,
	// rotation-corrected) mm for layout/hit-test.
	{
		float px = finger.x * (float)windowW;
		float py = finger.y * (float)windowH;
		float visX, visY;

		Aurora_TouchUI_WindowToVisual( px, py, windowW, windowH, &visX, &visY );
		vx = visX / touchPixelsPerMm;
		vy = visY / touchPixelsPerMm;
	}

	if ( !Aurora_TouchUI_Enabled() || !Aurora_TouchUI_InGameplay() )
	{
		// The overlay is not shown - still let go of anything this finger
		// held, so hiding mid-gesture cannot leave a button stuck down.
		if ( ev->type == SDL_FINGERUP )
		{
			for ( i = 0; i < TB_COUNT; i++ )
			{
				if ( touchButtons[i].held && touchButtons[i].finger == finger.fingerId )
				{
					Aurora_TouchUI_ReleaseButton( &touchButtons[i] );
				}
			}
			if ( touchStick.held && touchStick.finger == finger.fingerId ) Aurora_TouchUI_StickStop();
			if ( touchPad.held && touchPad.finger == finger.fingerId ) touchPad.held = 0;
		}
		return;
	}

	if ( ev->type == SDL_FINGERDOWN )
	{
		// buttons take priority over the stick/pad zones
		for ( i = 0; i < TB_COUNT; i++ )
		{
			touchButton_t *b = &touchButtons[i];
			float dx = vx - b->cx, dy = vy - b->cy;

			if ( b->held ) continue;
			if ( dx * dx + dy * dy > b->radiusMm * b->radiusMm ) continue;

			Aurora_TouchUI_PressButton( b, finger.fingerId );
			return;
		}

		{
			const float margin = cl_touchUIEdgeMargin->value;
			int visualW, visualH;
			float visualWmm, visualHmm;

			Aurora_TouchUI_VisualSize( windowW, windowH, &visualW, &visualH );
			visualWmm = (float)visualW / touchPixelsPerMm;
			visualHmm = (float)visualH / touchPixelsPerMm;

			if ( vx < margin || vx > visualWmm - margin || vy < margin || vy > visualHmm - margin )
			{
				return;
			}

			if ( vx < visualWmm * 0.5f )
			{
				if ( !touchStick.held )
				{
					touchStick.held = 1;
					touchStick.finger = finger.fingerId;
					touchStick.baseX = touchStick.curX = vx;
					touchStick.baseY = touchStick.curY = vy;
					touchStick.side = touchStick.forward = 0.0f;
				}
			}
			else
			{
				if ( !touchPad.held )
				{
					touchPad.held = 1;
					touchPad.finger = finger.fingerId;
					touchPad.lastX = finger.x * (float)windowW;
					touchPad.lastY = finger.y * (float)windowH;
					touchPad.remX = touchPad.remY = 0.0f;
				}
			}
		}
		return;
	}

	if ( ev->type == SDL_FINGERMOTION )
	{
		if ( touchStick.held && touchStick.finger == finger.fingerId )
		{
			Aurora_TouchUI_StickMove( vx, vy );
			return;
		}
		if ( touchPad.held && touchPad.finger == finger.fingerId )
		{
			Aurora_TouchUI_PadMove( finger.x * (float)windowW, finger.y * (float)windowH );
			return;
		}
		// buttons don't track finger motion (no TBF_LOOK-while-held in this
		// port's action set) - nothing else to do.
		return;
	}

	// SDL_FINGERUP
	for ( i = 0; i < TB_COUNT; i++ )
	{
		if ( touchButtons[i].held && touchButtons[i].finger == finger.fingerId )
		{
			Aurora_TouchUI_ReleaseButton( &touchButtons[i] );
		}
	}
	if ( touchStick.held && touchStick.finger == finger.fingerId )
	{
		Aurora_TouchUI_StickStop();
	}
	if ( touchPad.held && touchPad.finger == finger.fingerId )
	{
		touchPad.held = 0;
	}
}

void Aurora_TouchUI_NotePhysicalInput( void )
{
	touchPhysicalOverride = 1;
}

void Aurora_TouchUI_NoteJoystickChange( void )
{
	touchJoystickConnected = ( SDL_NumJoysticks() > 0 );
	if ( touchJoystickConnected )
	{
		touchPhysicalOverride = 1;
	}
}

/*
=================
Aurora_TouchUI_Frame
=================
*/
void Aurora_TouchUI_Frame( void )
{
	int enabled, gameplay, i;
	auroraTouchOverlay_t overlay;
	int windowW, windowH;

	Aurora_TouchUI_Layout();
	windowW = touchLayoutWindowW;
	windowH = touchLayoutWindowH;

	enabled = Aurora_TouchUI_Enabled();
	gameplay = Aurora_TouchUI_InGameplay();

	if ( !enabled || !gameplay )
	{
		// Whatever is no longer on screen lets go, exactly as if every
		// finger had just lifted - a menu opening or the console coming
		// down mid-drag must not leave anything stuck.
		for ( i = 0; i < TB_COUNT; i++ )
		{
			Aurora_TouchUI_ReleaseButton( &touchButtons[i] );
		}
		Aurora_TouchUI_StickStop();
		touchPad.held = 0;

		if ( re.Aurora_SetTouchOverlay )
		{
			re.Aurora_SetTouchOverlay( NULL );
		}
		return;
	}

	Aurora_TouchUI_StickApply();

	Com_Memset( &overlay, 0, sizeof( overlay ) );
	overlay.windowWidth = windowW;
	overlay.windowHeight = windowH;
	overlay.pixelsPerMm = touchPixelsPerMm;
	overlay.alpha = cl_touchUIAlpha->value;

	overlay.numButtons = TB_COUNT;
	if ( overlay.numButtons > AURORA_TOUCH_MAX_BUTTONS )
	{
		overlay.numButtons = AURORA_TOUCH_MAX_BUTTONS;
	}
	for ( i = 0; i < overlay.numButtons; i++ )
	{
		const touchButton_t *b = &touchButtons[i];
		float wx, wy;

		Aurora_TouchUI_VisualToWindow( b->cx * touchPixelsPerMm, b->cy * touchPixelsPerMm, windowW, windowH, &wx, &wy );

		overlay.buttons[i].x = wx;
		overlay.buttons[i].y = wy;
		overlay.buttons[i].radius = b->radiusMm * touchPixelsPerMm;
		overlay.buttons[i].icon = b->icon;
		overlay.buttons[i].pressed = b->held || b->latched;
	}

	if ( touchStick.held )
	{
		float wx, wy, kx, ky;

		Aurora_TouchUI_VisualToWindow( touchStick.baseX * touchPixelsPerMm, touchStick.baseY * touchPixelsPerMm, windowW, windowH, &wx, &wy );
		Aurora_TouchUI_VisualToWindow( touchStick.curX * touchPixelsPerMm, touchStick.curY * touchPixelsPerMm, windowW, windowH, &kx, &ky );

		overlay.stick = 1;
		overlay.stickBaseX = wx;
		overlay.stickBaseY = wy;
		overlay.stickKnobX = kx;
		overlay.stickKnobY = ky;
		overlay.stickRadius = cl_touchUIStickRadius->value * touchPixelsPerMm;
	}

	if ( re.Aurora_SetTouchOverlay )
	{
		re.Aurora_SetTouchOverlay( &overlay );
	}
}

void Aurora_TouchUI_Init( void )
{
	cl_touchUI = Cvar_Get( "cl_touchUI", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_touchUI, 0.0f, 2.0f, qtrue );
	cl_touchUIAlpha = Cvar_Get( "cl_touchUIAlpha", "0.6", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_touchUIAlpha, 0.1f, 1.0f, qfalse );
	cl_touchUIButtonSize = Cvar_Get( "cl_touchUIButtonSize", "11", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_touchUIButtonSize, 6.0f, 25.0f, qfalse );
	cl_touchUIEdgeMargin = Cvar_Get( "cl_touchUIEdgeMargin", "3", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_touchUIEdgeMargin, 0.0f, 20.0f, qfalse );
	cl_touchUIStickRadius = Cvar_Get( "cl_touchUIStickRadius", "12", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_touchUIStickRadius, 4.0f, 40.0f, qfalse );
	cl_touchUIStickDeadZone = Cvar_Get( "cl_touchUIStickDeadZone", "1.5", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_touchUIStickDeadZone, 0.0f, 10.0f, qfalse );
	cl_touchUILookSpeed = Cvar_Get( "cl_touchUILookSpeed", "32", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_touchUILookSpeed, 1.0f, 200.0f, qfalse );

	Com_Memset( &touchStick, 0, sizeof( touchStick ) );
	Com_Memset( &touchPad, 0, sizeof( touchPad ) );
	touchLayoutWindowW = touchLayoutWindowH = 0;

	// A gamepad may already be connected at launch - IN_Init calls this
	// after making sure SDL_INIT_JOYSTICK is up (see the caller).
	touchJoystickConnected = ( SDL_NumJoysticks() > 0 );
}

void Aurora_TouchUI_Shutdown( void )
{
	int i;

	for ( i = 0; i < TB_COUNT; i++ )
	{
		Aurora_TouchUI_ReleaseButton( &touchButtons[i] );
	}
	Aurora_TouchUI_StickStop();
	touchPad.held = 0;

	if ( re.Aurora_SetTouchOverlay )
	{
		re.Aurora_SetTouchOverlay( NULL );
	}
}

#else // !AURORA

void Aurora_TouchUI_Init( void ) {}
void Aurora_TouchUI_Shutdown( void ) {}
void Aurora_TouchUI_Frame( void ) {}
void Aurora_TouchUI_FingerEvent( const SDL_Event *ev ) {}
void Aurora_TouchUI_NotePhysicalInput( void ) {}
void Aurora_TouchUI_NoteJoystickChange( void ) {}

#endif // AURORA
