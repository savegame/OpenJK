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
#include "sdl_stickmath.h"

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
static cvar_t *cl_touchUIStickCurve;		// 0 linear, 1 quadratic response, stick move magnitude only
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
	TB_SAVE,
	TB_LOAD,
	TB_FORCE_PREV,	// bottom centre, one-shot - forceprev
	TB_FORCE_USE,	// bottom centre, held like fire - +useforce/-useforce
	TB_FORCE_NEXT,	// bottom centre, one-shot - forcenext
	TB_COUNT
} touchButtonIndex_t;

typedef struct {
	const char			*downCmd;	// "+attack\n" etc - NULL for the menu button (sends A_ESCAPE instead)
	const char			*upCmd;		// "-attack\n" - NULL for the menu button and for oneShot buttons
	auroraTouchIcon_t	icon;		// ignored by the renderer when label != NULL
	const char			*label;		// NULL: draw `icon` (see tr_touchui.h's comment). Non-NULL:
								// draw this text instead - a static string literal (SAVE/LOAD).
	int					latch;		// 1: a tap toggles it on, the next tap off (crouch)
	int					oneShot;	// 1: a tap fires downCmd once, nothing on release (quicksave/
								// quickload - stateless engine commands, not a held +action)

	// runtime state
	int			held;
	SDL_FingerID	finger;
	int			down;		// action currently applied (for non-latch, non-oneShot buttons)
	int			latched;	// latch currently on (for latch buttons)

	// Fix (user report): a finger holding an action button must not block
	// camera look - it keeps driving the pad with its own delta, exactly
	// like touchPad, while the button stays held. Not used by the menu
	// button (downCmd == NULL) or by oneShot buttons (a tap is instant,
	// there is nothing meaningful to drag).
	float		lookLastX, lookLastY;	// WINDOW px, last processed point for this finger
	float		lookRemX, lookRemY;		// sub-pixel carry, mirrors touchPad.remX/Y

	// layout, VISUAL mm (see Aurora_TouchUI_Layout) - centre, plus either a
	// circle radius (icon button, label == NULL) or a capsule's half-
	// extents (text button, label != NULL - see Aurora_TouchUI_CapsuleWidthMm).
	float		cx, cy, radiusMm;
	float		capHalfWMm, capHalfHMm;
} touchButton_t;

static touchButton_t touchButtons[TB_COUNT] = {
	{ NULL,           NULL,            AURORA_TOUCH_ICON_MENU,    NULL,   0, 0 },
	{ "+attack\n",    "-attack\n",     AURORA_TOUCH_ICON_FIRE,    NULL,   0, 0 },
	{ "+altattack\n", "-altattack\n",  AURORA_TOUCH_ICON_ALTFIRE, NULL,   0, 0 },
	{ "+moveup\n",    "-moveup\n",     AURORA_TOUCH_ICON_JUMP,    NULL,   0, 0 },
	{ "+movedown\n",  "-movedown\n",   AURORA_TOUCH_ICON_CROUCH,  NULL,   0, 0 },
	{ "+use\n",       "-use\n",        AURORA_TOUCH_ICON_USE,     NULL,   0, 0 },
	// Quicksave/quickload (port task B-003): "save quick"/"load quick" is the
	// exact slot name JKA's own in-game menu uses for these
	// (code/ui/ui_main.cpp's "savegame"/"load_quick" handlers and
	// code/ui/ui_shared.cpp's always-allowed command list both spell it this
	// way for non-JK2_MODE builds) - sharing it means a save made from this
	// button loads correctly from the game's own quickload menu entry and
	// vice versa. One-shot: no held state, no -command.
	{ "save quick\n", NULL,            AURORA_TOUCH_ICON_MENU,    "SAVE", 0, 1 },
	{ "load quick\n", NULL,            AURORA_TOUCH_ICON_MENU,    "LOAD", 0, 1 },

	// Force powers (port task B-002): the engine has no single "use force"
	// key of its own - forceprev/forcenext (stateless console commands,
	// CG_PrevForcePower_f/CG_NextForcePower_f, cg_consolecmds.cpp:229-230)
	// cycle which power is selected, +useforce/-useforce
	// (IN_Button2Down/Up, cl_input.cpp:1090-1091) activates whichever one
	// currently is, exactly like +attack does for the weapon. Prev/next
	// are one-shot taps for the same reason SAVE/LOAD are above (a
	// stateless command, nothing meaningful to hold); use is a normal
	// held button, so it drains a charged-hold power (heal, speed, grip,
	// ...) for exactly as long as the finger stays down, same as a real
	// bound key would.
	//
	// Indicator: cg.forcepowerSelect (code/cgame/cg_local.h) is the
	// currently-selected power, but it lives in the SPGame DLL, not this
	// executable (research/touch_ui_research.md п.5's dlopen boundary) -
	// bridging it out would need a new cvar (same pattern as
	// cg_inCameraCutscene below) AND a way to show it on a CIRCULAR
	// button without turning it into a capsule, which the port's own
	// rule keeps for action buttons (round stays round, only SAVE/LOAD/
	// SKIP are capsules - see tr_touchui.h's comment: a capsule's
	// width/height swap at 90/270, a circle's radius doesn't, which is
	// why every OTHER action button in this array is a circle). Drawing
	// per-power glyphs (15 of them, forcePowers_t, q_shared.h) or text
	// inside a fixed-radius circle would need a new draw primitive this
	// task's budget does not call for - left unindicated, as the task
	// allows when a cheap option isn't available.
	{ "forceprev\n",  NULL,            AURORA_TOUCH_ICON_FORCE_PREV, NULL, 0, 1 },
	{ "+useforce\n",  "-useforce\n",   AURORA_TOUCH_ICON_FORCE,      NULL, 0, 0 },
	{ "forcenext\n",  NULL,            AURORA_TOUCH_ICON_FORCE_NEXT, NULL, 0, 1 },
};

// ---------------------------------------------------------------------------
// stick (left half) and camera pad (right half)
// ---------------------------------------------------------------------------

static struct {
	int				held;
	SDL_FingerID	finger;
	float			baseX, baseY;	// VISUAL mm, where the finger landed
	float			curX, curY;		// VISUAL mm, current finger position (clamped to radius for the knob)

	// Analog move - dead-zone-rescaled and (per cl_touchUIStickCurve)
	// curve-shaped by the shared Aurora_Stick_Radial (sdl_stickmath.h),
	// exactly like the gamepad's left stick (sdl_gamepad.cpp). Read once
	// per usercmd by Aurora_TouchUI_GetMove(), not applied here - this
	// struct only tracks the finger, it never touches usercmd_t itself.
	float			side, forward;	// -1..1, movement axes
	float			magnitude;		// 0..1, same rescale, direction-independent
	qboolean		active;			// side/forward/magnitude past the dead zone
} touchStick;

static struct {
	int				held;
	SDL_FingerID	finger;
	float			lastX, lastY;	// WINDOW px (native, matches SDL_MOUSEMOTION's own space)
	float			remX, remY;		// FBO-local px, sub-pixel carry - mirrors sdl_input.cpp's aurora_touch.remX/Y
} touchPad;

// ---------------------------------------------------------------------------
// cinematic skip (port task B-003) - top-left, the same corner the menu
// button occupies during normal gameplay (its top-left EDGE, not its
// centre, is what actually lines up - see Aurora_TouchUI_Layout: laid out
// right after TB_MENU). A separate control, not an entry in touchButtons[]:
// it has no +/- command pair, no held/latch state worth keeping between
// frames, and its action depends on WHICH kind of cinematic is currently
// playing, decided at press time (see Aurora_TouchUI_SkipCinematic) rather
// than a fixed command string.
// ---------------------------------------------------------------------------

static struct {
	int				held;
	SDL_FingerID	finger;
	float			cx, cy;				// VISUAL mm, capsule centre
	float			capHalfWMm, capHalfHMm;	// VISUAL mm, capsule half-extents
} touchSkip;

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

// A word in capitals is guessed at half its em height wide per letter, an
// 'I' a quarter - erring on the wide side on purpose, so whatever the
// renderer's real font metrics come out to (Aurora_TouchUI_AddLabel,
// gles3_touchui_draw.cpp) never overflows the capsule this lays out from
// the estimate. This module cannot ask the renderer for the real width:
// its ImGui font lives in the rd-gles3 module, a separate SHARED library
// the client Sys_LoadDll's, not something this file can call into
// (research/touch_ui_research.md п.5) - same reasoning as the reference
// this look is matched to, quake4es
// (renderer/TouchOverlay.h's TouchOverlay_TextWidth/CapsuleWidth), which
// hit the identical constraint and settled on the identical estimate.
#define AURORA_TOUCH_FONT_HEIGHT_FRACTION	0.5f	// of the capsule's height
#define AURORA_TOUCH_TEXT_PADDING_FRACTION	0.45f	// per side, of the capsule's height

static float Aurora_TouchUI_TextWidthMm( const char *text, float fontHeightMm )
{
	float width = 0.0f;

	for ( ; *text; text++ )
	{
		width += ( *text == 'I' ) ? 0.25f * fontHeightMm : 0.5f * fontHeightMm;
	}
	return width;
}

static float Aurora_TouchUI_CapsuleWidthMm( const char *text, float heightMm )
{
	return Aurora_TouchUI_TextWidthMm( text, AURORA_TOUCH_FONT_HEIGHT_FRACTION * heightMm ) + 2.0f * AURORA_TOUCH_TEXT_PADDING_FRACTION * heightMm;
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

	// SAVE/LOAD/SKIP (port task B-003) are capsules, not circles - as wide
	// as their word needs (Aurora_TouchUI_CapsuleWidthMm), matching the
	// look of the reference quake4es port (~/Projects/aurora-ports/quake4/
	// quake4es, renderer/TouchOverlay.h + sys/sdl/touch_ui.cpp) the user
	// asked to match pixel-for-shape. All three share one height so they
	// read as the same kind of control wherever they appear.
	{
		const float capHmm = smallMm * 0.7f;
		float skipWmm, saveWmm, loadWmm;

		// The cinematic-skip control takes over the menu button's corner
		// whenever it is shown (Aurora_TouchUI_Frame only ever sends one or
		// the other, never both - see Aurora_TouchUI_CinematicSkippable) -
		// "слева вверху", same top-left EDGE the menu button's circle
		// touches, sized for its own word rather than the menu icon's circle.
		skipWmm = Aurora_TouchUI_CapsuleWidthMm( "SKIP", capHmm );
		touchSkip.capHalfWMm = skipWmm * 0.5f;
		touchSkip.capHalfHMm = capHmm * 0.5f;
		touchSkip.cx = margin + skipWmm * 0.5f;
		touchSkip.cy = margin + capHmm * 0.5f;

		// Top-centre: flanking the visual midline, clear of both the menu
		// button (top-left) and USE (top-right, laid out below) at any
		// reasonable screen width.
		saveWmm = Aurora_TouchUI_CapsuleWidthMm( "SAVE", capHmm );
		loadWmm = Aurora_TouchUI_CapsuleWidthMm( "LOAD", capHmm );

		touchButtons[TB_SAVE].capHalfWMm = saveWmm * 0.5f;
		touchButtons[TB_SAVE].capHalfHMm = capHmm * 0.5f;
		touchButtons[TB_SAVE].cx = visualWmm * 0.5f - gapMm * 0.5f - saveWmm * 0.5f;
		touchButtons[TB_SAVE].cy = margin + capHmm * 0.5f;

		touchButtons[TB_LOAD].capHalfWMm = loadWmm * 0.5f;
		touchButtons[TB_LOAD].capHalfHMm = capHmm * 0.5f;
		touchButtons[TB_LOAD].cx = visualWmm * 0.5f + gapMm * 0.5f + loadWmm * 0.5f;
		touchButtons[TB_LOAD].cy = touchButtons[TB_SAVE].cy;
	}

	// Bottom-right cluster: fire is the large central button (the thumb's
	// resting position), jump above it, crouch to its left, alt-fire on the
	// upper-left diagonal, use further out to the left - mirrors the
	// reference's "large primary action, flanked by smaller utility
	// buttons, thumb never has to leave the corner" shape.
	touchButtons[TB_FIRE].radiusMm = bigMm * 0.5f;
	touchButtons[TB_FIRE].cx = right - bigMm * 1.45f;
	touchButtons[TB_FIRE].cy = bottom - bigMm * 2.0f;

	touchButtons[TB_JUMP].radiusMm = smallMm * 0.5f;
	touchButtons[TB_JUMP].cx = right - smallMm * 0.3f;
	touchButtons[TB_JUMP].cy = bottom - smallMm * 3.5f;

	touchButtons[TB_CROUCH].radiusMm = smallMm * 0.5f;
	touchButtons[TB_CROUCH].cx = touchButtons[TB_FIRE].cx - gapMm;
	touchButtons[TB_CROUCH].cy = touchButtons[TB_FIRE].cy - smallMm - gapMm;

	touchButtons[TB_ALTFIRE].radiusMm = smallMm * 0.5f;
	touchButtons[TB_ALTFIRE].cx = right - smallMm * 0.3f;
	touchButtons[TB_ALTFIRE].cy = touchButtons[TB_FIRE].cy - smallMm - gapMm ;

	touchButtons[TB_USE].radiusMm = smallMm * 0.5f;
	touchButtons[TB_USE].cx = touchButtons[TB_JUMP].cx;
	touchButtons[TB_USE].cy = smallMm * 0.5f;

	// Force prev/use/next (port task B-002): bottom centre, three small
	// circles in a row - "use" in the middle (same size as the other
	// primary held actions), flanked by the two one-shot cycle buttons.
	// Its own row, below the stick/pad's usual working range and well
	// clear of both the SAVE/LOAD row (top centre) and the fire cluster
	// (right) at any reasonable screen size - see the layout comment atop
	// this function for why edges stay clear (margin) the same way.
	touchButtons[TB_FORCE_USE].radiusMm = smallMm * 0.5f;
	touchButtons[TB_FORCE_USE].cx = visualWmm * 0.5f;
	touchButtons[TB_FORCE_USE].cy = bottom - smallMm * 0.5f;

	touchButtons[TB_FORCE_PREV].radiusMm = smallMm * 0.5f;
	touchButtons[TB_FORCE_PREV].cx = touchButtons[TB_FORCE_USE].cx - smallMm - gapMm;
	touchButtons[TB_FORCE_PREV].cy = touchButtons[TB_FORCE_USE].cy;

	touchButtons[TB_FORCE_NEXT].radiusMm = smallMm * 0.5f;
	touchButtons[TB_FORCE_NEXT].cx = touchButtons[TB_FORCE_USE].cx + smallMm + gapMm;
	touchButtons[TB_FORCE_NEXT].cy = touchButtons[TB_FORCE_USE].cy;
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
Aurora_TouchUI_RoqCinematicActive / CameraCutsceneActive / CinematicActive / CinematicSkippable

Port task B-003, "kнопка skip ... на всех игровых роликах, видео и
катсценах". JKA (SP) has two unrelated mechanisms that both count as "a
cinematic is playing", and this module can only see one of them directly:

  - Full-screen/in-game ROQ video (cls.state == CA_CINEMATIC or
    CL_IsRunningInGameCinematic()) is entirely client-engine state - this
    file is compiled into the same executable as cl_cin.cpp, so `cls` and
    CL_IsRunningInGameCinematic() (client/client.h) are directly readable.
    This is the EXACT condition CL_KeyDownEvent (cl_keys.cpp) already uses
    to decide "any key skips the video" - matched 1:1 on purpose (see
    Aurora_TouchUI_SkipCinematic).

  - Scripted in-engine camera cutscenes (ICARUS CAM_* moving client_camera,
    code/cgame/cg_camera.cpp's `in_camera`) are cgame-DLL state: SPGame is
    a separate SHARED library (code/game/CMakeLists.txt) the client loads
    via Sys_LoadDll, so `in_camera` itself is not an address this file can
    read. cg_camera.cpp bridges it out through a plain cvar
    ("cg_inCameraCutscene", CGCam_Enable/Disable, cg_main.cpp's CG_Shutdown
    for the safety-net reset) - the same kind of cross-module cvar bridge
    already used elsewhere in this port (cl_auroraFboScale) and by the
    engine itself (skippingCinematic, independently Cvar_Get by
    cl_main.cpp/cg_main.cpp/g_main.cpp/common.cpp - reading a cvar nobody
    local registered first is normal here, Cvar_VariableIntegerValue
    returns 0 for an unknown name).

CinematicSkippable additionally requires !Key_GetCatcher(): if the console
or a UI menu is up, CL_KeyDownEvent's own cinematic-skip branch does NOT
fire (keys go to console/UI instead) - the skip button must not claim to
do something the engine itself would not do from the same input right now.
=================
*/
static int Aurora_TouchUI_RoqCinematicActive( void )
{
	return cls.state == CA_CINEMATIC || CL_IsRunningInGameCinematic();
}

static int Aurora_TouchUI_CameraCutsceneActive( void )
{
	return Cvar_VariableIntegerValue( "cg_inCameraCutscene" ) != 0;
}

static int Aurora_TouchUI_CinematicActive( void )
{
	return Aurora_TouchUI_RoqCinematicActive() || Aurora_TouchUI_CameraCutsceneActive();
}

static int Aurora_TouchUI_CinematicSkippable( void )
{
	return Aurora_TouchUI_CinematicActive() && !Key_GetCatcher();
}

/*
=================
Aurora_TouchUI_SkipCinematic

The actual skip action, dispatched on WHICH kind is currently playing
(favouring the ROQ path if - implausibly - both were true at once, since
that is the one with an explicit start-of-playback grace period to
respect). Deliberately calls the same engine entry points the vanilla
Escape/use-button skip already uses, not a hand-rolled stop - see the two
branches below for exactly which, and why.
=================
*/
static void Aurora_TouchUI_SkipCinematic( void )
{
	if ( Aurora_TouchUI_RoqCinematicActive() )
	{
		// Exactly what CL_KeyDownEvent (cl_keys.cpp) does for ANY key while
		// cls.state == CA_CINEMATIC or CL_IsRunningInGameCinematic(): both
		// the full-screen intro/outro movies and an in-game ROQ played over
		// gameplay stop through this one call. bAllowRefusal=qtrue keeps
		// the vanilla ~1.2s grace period (SCR_StopCinematic, cl_cin.cpp) so
		// a stray tap right as playback starts can't skip it prematurely -
		// same as a real Escape press would behave.
		SCR_StopCinematic( qtrue );
		return;
	}

	if ( Aurora_TouchUI_CameraCutsceneActive() )
	{
		// Mirrors the "exitview" server command JKA's own UI issues for
		// exactly this purpose (code/ui/ui_shared.cpp's always-allowed
		// command list) - cls.state == CA_ACTIVE here (a camera cutscene
		// is gameplay state, just rendered differently), so this reaches
		// the game DLL's Svcmd_ExitView_f (code/game/g_svcmds.cpp) via the
		// normal "unknown client command -> forward to server" path
		// (CL_ForwardCommandToServer, cl_main.cpp). That toggles
		// G_StartCinematicSkip()/G_StopCinematicSkip() (g_active.cpp):
		// fast-forwards the ICARUS camera script (timescale 100) until it
		// ends on its own - exactly what holding the "use" button already
		// does mid-cutscene (ClientCinematicThink, same file). This is a
		// fast-forward, not a jump-cut: it is the only skip primitive
		// JKA's own scripting exposes for this mechanism - see this task's
		// write-up for why a truer instant-skip isn't available here.
		Cbuf_ExecuteText( EXEC_APPEND, "exitview\n" );
	}
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

	if ( b->oneShot )
	{
		// Quicksave/quickload: fire the command once on the tap; b->down/
		// b->latched are deliberately left at 0 so Aurora_TouchUI_ReleaseButton
		// finds nothing to undo on FINGERUP (there is no "-save").
		Cbuf_ExecuteText( EXEC_APPEND, b->downCmd );
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
	touchStick.side = touchStick.forward = touchStick.magnitude = 0.0f;
	touchStick.active = qfalse;
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
	}
	touchStick.curX = touchStick.baseX + dx;
	touchStick.curY = touchStick.baseY + dy;

	// Normalize to the stick's own travel radius (radiusMm == full
	// deflection) and reuse the exact radial dead-zone + curve math the
	// gamepad's left stick uses (Aurora_Stick_Radial, sdl_stickmath.h) -
	// the dead zone expressed as a fraction of that same radius; screen-
	// down is negative forward.
	touchStick.active = Aurora_Stick_Radial( dx / radiusMm, -dy / radiusMm, deadMm / radiusMm,
		(qboolean)cl_touchUIStickCurve->integer,
		&touchStick.side, &touchStick.forward, &touchStick.magnitude );
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

// Shared by the camera pad (touchPad) and by any action button held while
// dragging (user report: a button must not block camera look - the finger
// that pressed it keeps turning the camera exactly like the pad, while the
// button itself stays held/latched). lastX/lastY/remX/remY are the
// caller's own per-finger state (touchPad's fields, or a touchButton_t's
// lookLastX/lookLastY/lookRemX/lookRemY).
static void Aurora_TouchUI_LookMove( float windowX, float windowY, float *lastX, float *lastY, float *remX, float *remY )
{
	const float mmPerPx = 1.0f / touchPixelsPerMm;
	const float unitsPerMm = cl_touchUILookSpeed->value;
	float dxWin = windowX - *lastX;
	float dyWin = windowY - *lastY;
	float dxUnits, dyUnits, tdx, tdy;
	int idx, idy;

	*lastX = windowX;
	*lastY = windowY;

	dxUnits = dxWin * mmPerPx * unitsPerMm;
	dyUnits = dyWin * mmPerPx * unitsPerMm;

	Aurora_TouchUI_RotateDelta( dxUnits, dyUnits, &tdx, &tdy );

	*remX += tdx;
	*remY += tdy;
	idx = (int)*remX;
	idy = (int)*remY;
	*remX -= (float)idx;
	*remY -= (float)idy;

	if ( idx || idy )
	{
		Sys_QueEvent( 0, SE_MOUSE, idx, idy, 0, NULL );
	}
}

static void Aurora_TouchUI_PadMove( float windowX, float windowY )
{
	Aurora_TouchUI_LookMove( windowX, windowY, &touchPad.lastX, &touchPad.lastY, &touchPad.remX, &touchPad.remY );
}

/*
=================
Aurora_TouchUI_ButtonHit

A capsule (label != NULL) hit-tests as its rectangle; an icon button
(label == NULL) as its circle, same as always. dx/dy are the touch point
relative to the button's centre, both in VISUAL mm (the same space
Aurora_TouchUI_Layout laid the button out in).
=================
*/
static int Aurora_TouchUI_ButtonHit( const touchButton_t *b, float dx, float dy )
{
	if ( b->label )
	{
		return fabsf( dx ) <= b->capHalfWMm && fabsf( dy ) <= b->capHalfHMm;
	}
	return dx * dx + dy * dy <= b->radiusMm * b->radiusMm;
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

	if ( Aurora_TouchUI_CinematicSkippable() )
	{
		// Skip-only mode (port task B-003): the rest of the overlay is not
		// drawn (see Aurora_TouchUI_Frame), so it must not react to touches
		// either - only the skip control, same slot as the menu button, is
		// live. Still respects Aurora_TouchUI_Enabled() (cl_touchUI 0, or a
		// physical input override) - "off" means fully off, skip included.
		if ( ev->type == SDL_FINGERDOWN && !touchSkip.held && Aurora_TouchUI_Enabled() )
		{
			float dx = vx - touchSkip.cx, dy = vy - touchSkip.cy;

			if ( fabsf( dx ) <= touchSkip.capHalfWMm && fabsf( dy ) <= touchSkip.capHalfHMm )
			{
				touchSkip.held = 1;
				touchSkip.finger = finger.fingerId;
				Aurora_TouchUI_SkipCinematic();
			}
		}
		else if ( ev->type == SDL_FINGERUP )
		{
			// Let go of anything else this finger might have been holding
			// right as the cinematic started, exactly like the "not shown"
			// branch below - a cinematic starting mid-gesture must not
			// leave a gameplay button/stick/pad stuck.
			for ( i = 0; i < TB_COUNT; i++ )
			{
				if ( touchButtons[i].held && touchButtons[i].finger == finger.fingerId )
				{
					Aurora_TouchUI_ReleaseButton( &touchButtons[i] );
				}
			}
			if ( touchStick.held && touchStick.finger == finger.fingerId ) Aurora_TouchUI_StickStop();
			if ( touchPad.held && touchPad.finger == finger.fingerId ) touchPad.held = 0;
			if ( touchSkip.held && touchSkip.finger == finger.fingerId ) touchSkip.held = 0;
		}
		return;
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
			if ( touchSkip.held && touchSkip.finger == finger.fingerId ) touchSkip.held = 0;
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
			if ( !Aurora_TouchUI_ButtonHit( b, dx, dy ) ) continue;

			Aurora_TouchUI_PressButton( b, finger.fingerId );

			// Fix (user report): the same finger also drives the camera
			// while it holds an action button - never for the menu button
			// (downCmd == NULL, sends Escape and nothing else) or for a
			// oneShot button (save/load - a tap is instant, no held drag).
			if ( b->downCmd && !b->oneShot )
			{
				b->lookLastX = finger.x * (float)windowW;
				b->lookLastY = finger.y * (float)windowH;
				b->lookRemX = b->lookRemY = 0.0f;
			}
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
					touchStick.side = touchStick.forward = touchStick.magnitude = 0.0f;
					touchStick.active = qfalse;
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

		// A finger holding an action button (fire/altfire/jump/crouch/use)
		// keeps turning the camera as it slides - the button stays held
		// regardless (see Aurora_TouchUI_ReleaseButton on FINGERUP). Not
		// for oneShot buttons (save/load) - see the FINGERDOWN comment above.
		for ( i = 0; i < TB_COUNT; i++ )
		{
			touchButton_t *b = &touchButtons[i];

			if ( b->held && b->finger == finger.fingerId && b->downCmd && !b->oneShot )
			{
				Aurora_TouchUI_LookMove( finger.x * (float)windowW, finger.y * (float)windowH,
					&b->lookLastX, &b->lookLastY, &b->lookRemX, &b->lookRemY );
				return;
			}
		}
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
	// Bug fix: this generic "normal gameplay" FINGERUP path was the only
	// one of the three per-frame states (see Aurora_TouchUI_Frame) that
	// never checked touchSkip at all - a FINGERUP delivered here (i.e.
	// while Aurora_TouchUI_CinematicSkippable() is momentarily false, the
	// gap between a camera cutscene ending and a following ROQ video
	// actually starting counts) left touchSkip.held stuck at 1 forever.
	// Symptom matched the user report exactly: SKIP visually stayed
	// "pressed" and Aurora_TouchUI_FingerEvent's `!touchSkip.held` guard
	// then silently refused every later tap, so the very next cinematic
	// (typically the video the camera cutscene cuts to) could no longer
	// be skipped at all. See also the frame-level safety net in
	// Aurora_TouchUI_Frame(), which additionally clears this regardless
	// of which finger last touched it.
	if ( touchSkip.held && touchSkip.finger == finger.fingerId )
	{
		touchSkip.held = 0;
	}
}

void Aurora_TouchUI_NotePhysicalInput( void )
{
	touchPhysicalOverride = 1;
}

/*
=================
Aurora_TouchUI_RealJoystickConnected

This device's own udev data tags the on-board "mtk-kpd" keypad node
(physical volume buttons) as ID_INPUT_JOYSTICK, and SDL - which
discovers joysticks through udev - reports that keypad as a joystick
for as long as it exists. With the naive "any joystick hides the
overlay" rule the auto-mode overlay would stay hidden forever:
SDL_NumJoysticks() is 1 from boot, no gamepad involved.

A real gamepad reaches a phone over USB or Bluetooth; the phantom
internal "joysticks" (on-board key pads, and - via SDL's evdev class
guesser, src/core/linux/SDL_evdev_capabilities.c - touch panels that
report a stray gamepad-range key) are internal bus nodes. SDL puts the
evdev bus type into the first two (LE) bytes of the joystick GUID, so
that is what we key on: only USB/Bluetooth joysticks count as a
physical gamepad.
=================
*/
static int Aurora_TouchUI_RealJoystickConnected( void )
{
	int i;

	for ( i = 0; i < SDL_NumJoysticks(); i++ )
	{
		const SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID( i );
		const unsigned int bus = guid.data[0] | ( (unsigned int)guid.data[1] << 8 );

		if ( bus == 0x03 /* BUS_USB */ || bus == 0x05 /* BUS_BLUETOOTH */ )
		{
			return qtrue;
		}
	}
	return qfalse;
}

void Aurora_TouchUI_NoteJoystickChange( void )
{
	touchJoystickConnected = Aurora_TouchUI_RealJoystickConnected();
	if ( touchJoystickConnected )
	{
		touchPhysicalOverride = 1;
	}
}

/*
=================
Aurora_TouchUI_GetMove

Mirrors Aurora_Gamepad_GetMove (sdl_gamepad.cpp) exactly - see
sdl_touchui.h for the contract. touchStick.side/forward/magnitude are
already dead-zone-rescaled and curve-shaped by Aurora_Stick_Radial as of
the last SDL_FINGERMOTION (Aurora_TouchUI_StickMove above); a finger held
still between motion events keeps reporting that same value every frame,
exactly like a physical stick held at a fixed deflection would.
=================
*/
qboolean Aurora_TouchUI_GetMove( int *forwardmove, int *rightmove, qboolean *walking )
{
	if ( !touchStick.held || !touchStick.active )
	{
		return qfalse;
	}

	*forwardmove = (int)( touchStick.forward * 127.0f );
	*rightmove = (int)( touchStick.side * 127.0f );
	*walking = (qboolean)( touchStick.magnitude < AURORA_STICK_RUN_FRACTION );
	return qtrue;
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

	// Bug fix (button-stuck report): force-clear touchSkip the moment
	// neither cinematic kind is active any more, independent of whether
	// its owning finger's FINGERUP was ever seen by
	// Aurora_TouchUI_FingerEvent - see the fix there for the exact gap
	// this closes (and why relying on FINGERUP delivery alone was not
	// enough: cls.state/CL_IsRunningInGameCinematic()/
	// cg_inCameraCutscene can all flip within a single frame, faster than
	// any finger event). Runs every frame, so a stale flag cannot survive
	// past the first frame the cinematic is truly over, whichever finger
	// last held it.
	if ( touchSkip.held && !Aurora_TouchUI_CinematicActive() )
	{
		touchSkip.held = 0;
	}

	if ( Aurora_TouchUI_CinematicSkippable() && Aurora_TouchUI_Enabled() )
	{
		// Skip-only overlay (port task B-003): every other control is
		// released (mirrors the hide path below) and NOT sent to the
		// renderer - only the skip button, top-left, same slot the menu
		// button uses in normal gameplay (see Aurora_TouchUI_Layout).
		for ( i = 0; i < TB_COUNT; i++ )
		{
			Aurora_TouchUI_ReleaseButton( &touchButtons[i] );
		}
		Aurora_TouchUI_StickStop();
		touchPad.held = 0;

		Com_Memset( &overlay, 0, sizeof( overlay ) );
		overlay.windowWidth = windowW;
		overlay.windowHeight = windowH;
		overlay.pixelsPerMm = touchPixelsPerMm;
		overlay.alpha = cl_touchUIAlpha->value;
		overlay.transform = Aurora_TouchUI_GetTransform();

		overlay.numButtons = 1;
		{
			float wx, wy;

			Aurora_TouchUI_VisualToWindow( touchSkip.cx * touchPixelsPerMm, touchSkip.cy * touchPixelsPerMm, windowW, windowH, &wx, &wy );

			overlay.buttons[0].x = wx;
			overlay.buttons[0].y = wy;
			overlay.buttons[0].halfWidth = touchSkip.capHalfWMm * touchPixelsPerMm;
			overlay.buttons[0].halfHeight = touchSkip.capHalfHMm * touchPixelsPerMm;
			overlay.buttons[0].label = "SKIP";
			overlay.buttons[0].pressed = touchSkip.held;
		}

		if ( re.Aurora_SetTouchOverlay )
		{
			re.Aurora_SetTouchOverlay( &overlay );
		}
		return;
	}

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
		touchSkip.held = 0;

		if ( re.Aurora_SetTouchOverlay )
		{
			re.Aurora_SetTouchOverlay( NULL );
		}
		return;
	}

	Com_Memset( &overlay, 0, sizeof( overlay ) );
	overlay.windowWidth = windowW;
	overlay.windowHeight = windowH;
	overlay.pixelsPerMm = touchPixelsPerMm;
	overlay.alpha = cl_touchUIAlpha->value;
	overlay.transform = Aurora_TouchUI_GetTransform();

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
		overlay.buttons[i].halfWidth = b->capHalfWMm * touchPixelsPerMm;
		overlay.buttons[i].halfHeight = b->capHalfHMm * touchPixelsPerMm;
		overlay.buttons[i].icon = b->icon;
		overlay.buttons[i].label = b->label;
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
	cl_touchUIStickCurve = Cvar_Get( "cl_touchUIStickCurve", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_touchUIStickCurve, 0.0f, 1.0f, qtrue );
	cl_touchUILookSpeed = Cvar_Get( "cl_touchUILookSpeed", "32", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_touchUILookSpeed, 1.0f, 200.0f, qfalse );

	Com_Memset( &touchStick, 0, sizeof( touchStick ) );
	Com_Memset( &touchPad, 0, sizeof( touchPad ) );
	Com_Memset( &touchSkip, 0, sizeof( touchSkip ) );
	touchLayoutWindowW = touchLayoutWindowH = 0;

	// A gamepad may already be connected at launch - IN_Init calls this
	// after making sure SDL_INIT_JOYSTICK is up (see the caller).
	touchJoystickConnected = Aurora_TouchUI_RealJoystickConnected();
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
	touchSkip.held = 0;

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
qboolean Aurora_TouchUI_GetMove( int *forwardmove, int *rightmove, qboolean *walking ) { return qfalse; }

#endif // AURORA
