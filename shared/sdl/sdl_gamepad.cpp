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

// Aurora gamepad support - see sdl_gamepad.h for the full design rationale
// (SDL_GameController vs. the legacy SDL_Joystick path, the JOYn keycode
// assignment, why the right stick alone is SE_MOUSE, why input is never
// rotated). This file only ever turns SDL controller state into
// Sys_QueEvent(SE_KEY, A_JOYn, ...) / Sys_QueEvent(SE_MOUSE, ...) calls -
// it never decides what a button *does*; that lives entirely in binds
// (see aurora_gamepad_defaults.cfg for the default layout).

#include <SDL.h>
#include <math.h>

#include "qcommon/qcommon.h"
#include "qcommon/q_shared.h"
#include "client/client.h"
#include "sys/sys_local.h"
#include "sdl_gamepad.h"
#include "sdl_stickmath.h"

#ifdef AURORA

// ---------------------------------------------------------------------------
// cvars - sensitivity/feel only, never layout (see sdl_gamepad.h)
// ---------------------------------------------------------------------------

static cvar_t *cl_gamepad;					// 0 off, 1 on (default) - master switch
static cvar_t *cl_gamepadDeadZone;			// 0..1 fraction of stick travel ignored, both sticks;
											// also the left stick's digitisation threshold
static cvar_t *cl_gamepadTriggerThreshold;	// 0..1 fraction of trigger travel counted as "pressed"
static cvar_t *cl_gamepadLookSpeed;		// mouse units/sec at full right-stick deflection, gameplay
static cvar_t *cl_gamepadMenuSpeed;		// mouse units/sec at full right-stick deflection, menu cursor
static cvar_t *cl_gamepadLookCurve;		// 0 linear, 1 quadratic response, gameplay look only
static cvar_t *cl_gamepadMoveCurve;		// 0 linear, 1 quadratic response, left-stick move magnitude only

static SDL_GameController *gamepad = NULL;
static SDL_JoystickID gamepadInstance = -1;

// ---------------------------------------------------------------------------
// JOYn slot assignment - see sdl_gamepad.h for the full rationale. Slots
// 0..20 mirror the SDL_GameControllerButton enum value directly; slots
// 21+ are synthetic digital signals derived from analog axes so the left
// stick and triggers stay bindable like any other button.
// ---------------------------------------------------------------------------

enum
{
	JOY_LEFTTRIGGER = 21,
	JOY_RIGHTTRIGGER = 22,
	JOY_LEFTSTICK_UP = 23,
	JOY_LEFTSTICK_DOWN = 24,
	JOY_LEFTSTICK_LEFT = 25,
	JOY_LEFTSTICK_RIGHT = 26,

	JOY_NUM_SLOTS = 32	// A_JOY0..A_JOY31
};

// Current reported state of every JOYn slot this module drives, purely for
// edge detection (only queue an event on an actual state change) and for
// cleanly releasing everything still "down" on disconnect/toggle-off.
static qboolean joyDown[JOY_NUM_SLOTS];

static void Aurora_Gamepad_SetJoy( int slot, qboolean down )
{
	if ( joyDown[slot] == down )
	{
		return;
	}
	joyDown[slot] = down;
	Sys_QueEvent( 0, SE_KEY, A_JOY0 + slot, down, 0, NULL );
}

static void Aurora_Gamepad_ReleaseAllJoy( void )
{
	int slot;

	for ( slot = 0; slot < JOY_NUM_SLOTS; slot++ )
	{
		if ( joyDown[slot] )
		{
			joyDown[slot] = qfalse;
			Sys_QueEvent( 0, SE_KEY, A_JOY0 + slot, qfalse, 0, NULL );
		}
	}
}

// ---------------------------------------------------------------------------
// state gates - only needed for the right stick's dual role (camera vs.
// menu cursor) and the A-button menu click. Ordinary buttons need none of
// this: their JOYn binds are already suppressed while a menu/console is
// catching keys by the engine's own CL_ParseBinding (cl_keys.cpp), exactly
// like a real keyboard/joystick key.
// ---------------------------------------------------------------------------

static int Aurora_Gamepad_MenuActive( void )
{
	return ( Key_GetCatcher() & KEYCATCH_UI ) != 0;
}

// Right stick doubles as the camera in gameplay and the cursor in the
// menu - active a little more broadly than "gameplay" (e.g. also with the
// console down), exactly like a real mouse still turns the camera with
// the console down.
static int Aurora_Gamepad_RightStickActive( void )
{
	if ( Aurora_Gamepad_MenuActive() ) return 1;
	return cls.state == CA_ACTIVE;
}

// ---------------------------------------------------------------------------
// buttons -> JOY0..JOY20, 1:1 with the SDL_GameControllerButton enum.
// Polled (not event-driven) so it shares one simple edge-detection helper
// with the axis-derived digital slots below, exactly like
// sdl_touchui.cpp's polling model.
// ---------------------------------------------------------------------------

static void Aurora_Gamepad_Buttons( void )
{
	int button;

	for ( button = 0; button < SDL_CONTROLLER_BUTTON_MAX && button < JOY_LEFTTRIGGER; button++ )
	{
		qboolean down = (qboolean)( SDL_GameControllerGetButton( gamepad, (SDL_GameControllerButton)button ) != 0 );
		Aurora_Gamepad_SetJoy( button, down );
	}
}

// ---------------------------------------------------------------------------
// triggers -> JOY_LEFTTRIGGER/JOY_RIGHTTRIGGER. No native "digital trigger"
// button exists on SDL_GameController - edge-detected against
// cl_gamepadTriggerThreshold, same idea the legacy joystick path uses for
// its own axis thresholds (sdl_input.cpp's in_joystickThreshold), just
// exposed as a bindable JOYn instead of a hardcoded action.
// ---------------------------------------------------------------------------

static void Aurora_Gamepad_Triggers( void )
{
	float rt, lt, threshold;

	threshold = cl_gamepadTriggerThreshold->value;
	rt = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT ) / 32767.0f;
	lt = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT ) / 32767.0f;

	Aurora_Gamepad_SetJoy( JOY_RIGHTTRIGGER, (qboolean)( rt > threshold ) );
	Aurora_Gamepad_SetJoy( JOY_LEFTTRIGGER,  (qboolean)( lt > threshold ) );
}

// ---------------------------------------------------------------------------
// left stick -> analog move (primary path) + four digital direction slots
// (optional fallback/rebind target only - see sdl_gamepad.h).
//
// Analog state is computed radially via the shared Aurora_Stick_Radial()
// (sdl_stickmath.h) - the same dead zone + (optional) response curve math
// the touch-UI's virtual stick uses (sdl_touchui.cpp) - so a diagonal push
// isn't shortchanged by a square dead zone, then the rescaled unit vector
// is stored for Aurora_Gamepad_GetMove() to hand to CL_AuroraGamepadMove()
// (code/client/cl_input.cpp) once per usercmd - this module only computes
// the numbers, it never touches usercmd_t itself.
//
// The four JOY_LEFTSTICK_* digital slots are kept exactly as before (same
// per-axis threshold against cl_gamepadDeadZone) purely so a player can
// still "bind JOY23 +forward" etc. if they want dpad-style digital
// movement back - they are simply no longer bound to anything by default
// (aurora_gamepad_defaults.cfg).
// ---------------------------------------------------------------------------

static float moveForward, moveRight;	// -1..1, dead-zone-rescaled and curved
static float moveMagnitude;			// 0..1, same rescale, direction-independent
static qboolean moveActive;			// mag was past the dead zone this frame

static void Aurora_Gamepad_LeftStick( void )
{
	float lx, ly, side, forward, dz;

	lx = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_LEFTX ) / 32767.0f;
	ly = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_LEFTY ) / 32767.0f;
	dz = cl_gamepadDeadZone->value;

	side = lx;
	forward = -ly;	// SDL's Y axis is positive-down; pushing the stick up is forward

	Aurora_Gamepad_SetJoy( JOY_LEFTSTICK_UP,    (qboolean)( forward >  dz ) );
	Aurora_Gamepad_SetJoy( JOY_LEFTSTICK_DOWN,  (qboolean)( forward < -dz ) );
	Aurora_Gamepad_SetJoy( JOY_LEFTSTICK_LEFT,  (qboolean)( side    < -dz ) );
	Aurora_Gamepad_SetJoy( JOY_LEFTSTICK_RIGHT, (qboolean)( side    >  dz ) );

	moveActive = Aurora_Stick_Radial( side, forward, dz, (qboolean)cl_gamepadMoveCurve->integer,
		&moveRight, &moveForward, &moveMagnitude );
}

// Walk/run split - see AURORA_STICK_RUN_FRACTION's own comment
// (sdl_stickmath.h), shared with the touch-UI's virtual stick.

qboolean Aurora_Gamepad_GetMove( int *forwardmove, int *rightmove, qboolean *walking )
{
	if ( !cl_gamepad->integer || !gamepad || !moveActive )
	{
		return qfalse;
	}

	*forwardmove = (int)( moveForward * 127.0f );
	*rightmove = (int)( moveRight * 127.0f );
	*walking = (qboolean)( moveMagnitude < AURORA_STICK_RUN_FRACTION );
	return qtrue;
}

// ---------------------------------------------------------------------------
// right stick -> camera look (gameplay) / cursor (menu). Synthesizes the
// same SE_MOUSE deltas a real SDL_MOUSEMOTION would produce - NOT passed
// through Aurora_TransformInputDelta*, per gameport/AGENTS.MD stage 4
// ("Ввод от геймпада... НЕ трансформируем"). Rate-based (scaled by frame
// time), unlike the touch camera pad which is driven by finger deltas -
// there is no finger here, only a continuous deflection to integrate.
//
// Gameplay and menu use independent speed cvars (cl_gamepadLookSpeed /
// cl_gamepadMenuSpeed): the same raw SE_MOUSE delta is scaled very
// differently downstream depending on the consumer (CL_MouseMove applies
// cl_sensitivity/m_yaw/m_pitch/FOV in gameplay; _UI_MouseEvent uses the
// menu cursor delta directly, unscaled) so one shared speed cannot feel
// right in both. Gameplay optionally applies a quadratic response curve
// (cl_gamepadLookCurve) for precise aim near center and fast turns at full
// deflection without changing the top speed; the menu cursor is always
// linear.
// ---------------------------------------------------------------------------

static float lookRemX, lookRemY;

static void Aurora_Gamepad_RightStick( void )
{
	float rx, ry, dz, mag, t;
	float unitsPerSec, dxf, dyf, scale;
	int dx, dy, menu;

	if ( !Aurora_Gamepad_RightStickActive() )
	{
		lookRemX = lookRemY = 0.0f;
		return;
	}

	rx = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_RIGHTX ) / 32767.0f;
	ry = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_RIGHTY ) / 32767.0f;
	dz = cl_gamepadDeadZone->value;

	mag = sqrtf( rx * rx + ry * ry );
	if ( mag < dz )
	{
		lookRemX = lookRemY = 0.0f;
		return;
	}

	// t in (0,1]: how far past the dead zone the stick is deflected
	t = ( mag - dz ) / ( 1.0f - dz );
	if ( t > 1.0f )
	{
		t = 1.0f;
	}

	menu = Aurora_Gamepad_MenuActive();

	if ( menu )
	{
		// Menu cursor: left exactly as it felt before this task (linear,
		// independent cvar/default from the gameplay look speed below).
		unitsPerSec = cl_gamepadMenuSpeed->value;
	}
	else
	{
		unitsPerSec = cl_gamepadLookSpeed->value;
		if ( cl_gamepadLookCurve->integer )
		{
			// Quadratic response: precise aim at small deflection, fast
			// turning at full deflection - same top speed either way
			// since t==1 maps to t==1 regardless.
			t = t * t;
		}
	}

	// Rescale the raw axis vector to the (possibly curved) response,
	// keeping direction, so response starts at 0 right past the dead
	// zone instead of jumping straight to (mag - dz)'s value.
	scale = t / mag;
	rx *= scale;
	ry *= scale;

	dxf = rx * unitsPerSec * ( (float)cls.realFrametime / 1000.0f ) + lookRemX;
	dyf = ry * unitsPerSec * ( (float)cls.realFrametime / 1000.0f ) + lookRemY;

	dx = (int)dxf;
	dy = (int)dyf;
	lookRemX = dxf - (float)dx;
	lookRemY = dyf - (float)dy;

	if ( dx || dy )
	{
		Sys_QueEvent( 0, SE_MOUSE, dx, dy, 0, NULL );
	}
}

// ---------------------------------------------------------------------------
// A button's second role: while a menu is showing, the press edge also
// clicks - synthesizing A_MOUSE1 (a mouse button code, not a keyboard
// keycode) press+release, exactly like the touch-UI trackpad's
// tap-to-click. This is standing in for "the button the UI already knows
// how to react to" (menus in this engine are driven by mouse/Enter, not by
// a generic bindable "activate" command), so it cannot be expressed as a
// JOYn bind - JOY0 itself is still sent as an ordinary bindable button by
// Aurora_Gamepad_Buttons() above (bound to +moveup/jump by default).
// ---------------------------------------------------------------------------

static qboolean wasAForClick;

static void Aurora_Gamepad_MenuClick( void )
{
	qboolean a = (qboolean)( SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_A ) != 0 );

	if ( Aurora_Gamepad_MenuActive() && a && !wasAForClick )
	{
		Sys_QueEvent( 0, SE_KEY, A_MOUSE1, qtrue, 0, NULL );
		Sys_QueEvent( 0, SE_KEY, A_MOUSE1, qfalse, 0, NULL );
	}
	wasAForClick = a;
}

// ---------------------------------------------------------------------------
// device lifecycle
// ---------------------------------------------------------------------------

static void Aurora_Gamepad_TryOpen( void )
{
	int i;

	if ( gamepad )
	{
		return;
	}

	for ( i = 0; i < SDL_NumJoysticks(); i++ )
	{
		if ( SDL_IsGameController( i ) )
		{
			gamepad = SDL_GameControllerOpen( i );
			if ( gamepad )
			{
				SDL_Joystick *j = SDL_GameControllerGetJoystick( gamepad );
				gamepadInstance = SDL_JoystickInstanceID( j );
				Com_Printf( "Aurora: gamepad connected (%s)\n", SDL_GameControllerName( gamepad ) );
				return;
			}
		}
	}
}

static void Aurora_Gamepad_Close( void )
{
	if ( !gamepad )
	{
		return;
	}

	Aurora_Gamepad_ReleaseAllJoy();
	SDL_GameControllerClose( gamepad );
	gamepad = NULL;
	gamepadInstance = -1;
	wasAForClick = qfalse;
	lookRemX = lookRemY = 0.0f;
	moveForward = moveRight = moveMagnitude = 0.0f;
	moveActive = qfalse;
}

void Aurora_Gamepad_Event( const SDL_Event *ev )
{
	if ( ev->type == SDL_CONTROLLERDEVICEADDED )
	{
		Aurora_Gamepad_TryOpen();
	}
	else if ( ev->type == SDL_CONTROLLERDEVICEREMOVED )
	{
		if ( gamepad && ev->cdevice.which == gamepadInstance )
		{
			Com_Printf( "Aurora: gamepad disconnected\n" );
			Aurora_Gamepad_Close();
			Aurora_Gamepad_TryOpen();	// another one may still be plugged in
		}
	}
}

/*
=================
Aurora_Gamepad_Frame
=================
*/
void Aurora_Gamepad_Frame( void )
{
	if ( !cl_gamepad->integer || !gamepad || !SDL_GameControllerGetAttached( gamepad ) )
	{
		Aurora_Gamepad_ReleaseAllJoy();
		return;
	}

	Aurora_Gamepad_Buttons();
	Aurora_Gamepad_Triggers();
	Aurora_Gamepad_LeftStick();
	Aurora_Gamepad_RightStick();
	Aurora_Gamepad_MenuClick();
}

void Aurora_Gamepad_Init( void )
{
	cl_gamepad = Cvar_Get( "cl_gamepad", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepad, 0.0f, 1.0f, qtrue );
	cl_gamepadDeadZone = Cvar_Get( "cl_gamepadDeadZone", "0.22", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepadDeadZone, 0.02f, 0.9f, qfalse );
	cl_gamepadTriggerThreshold = Cvar_Get( "cl_gamepadTriggerThreshold", "0.25", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepadTriggerThreshold, 0.05f, 0.95f, qfalse );
	// Gameplay look: raised well past the old shared default (250) per
	// on-device feedback that camera turn was much too slow once routed
	// through CL_MouseMove's cl_sensitivity/m_yaw scaling; still an
	// archived cvar so it can be tuned without a rebuild.
	cl_gamepadLookSpeed = Cvar_Get( "cl_gamepadLookSpeed", "3000", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepadLookSpeed, 10.0f, 50000.0f, qfalse );
	// Menu cursor: kept at the old shared default - on-device feedback was
	// that this speed already felt right, it must not move when the
	// gameplay default above changes.
	cl_gamepadMenuSpeed = Cvar_Get( "cl_gamepadMenuSpeed", "250", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepadMenuSpeed, 10.0f, 4000.0f, qfalse );
	cl_gamepadLookCurve = Cvar_Get( "cl_gamepadLookCurve", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepadLookCurve, 0.0f, 1.0f, qtrue );
	cl_gamepadMoveCurve = Cvar_Get( "cl_gamepadMoveCurve", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepadMoveCurve, 0.0f, 1.0f, qtrue );

	if ( !SDL_WasInit( SDL_INIT_GAMECONTROLLER ) )
	{
		SDL_InitSubSystem( SDL_INIT_GAMECONTROLLER );
	}

	// A controller already plugged in at launch - SDL_IsGameController
	// works immediately once the subsystem above is up, no hotplug event
	// needed for this first check (mirrors sdl_touchui.cpp's own
	// Aurora_TouchUI_RealJoystickConnected call from Aurora_TouchUI_Init).
	Aurora_Gamepad_TryOpen();
}

void Aurora_Gamepad_Shutdown( void )
{
	Aurora_Gamepad_Close();

	if ( SDL_WasInit( SDL_INIT_GAMECONTROLLER ) )
	{
		SDL_QuitSubSystem( SDL_INIT_GAMECONTROLLER );
	}
}

#else // !AURORA

void Aurora_Gamepad_Init( void ) {}
void Aurora_Gamepad_Shutdown( void ) {}
void Aurora_Gamepad_Frame( void ) {}
void Aurora_Gamepad_Event( const SDL_Event *ev ) {}
qboolean Aurora_Gamepad_GetMove( int *forwardmove, int *rightmove, qboolean *walking ) { return qfalse; }

#endif // AURORA
