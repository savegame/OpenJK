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
// (SDL_GameController vs. the legacy SDL_Joystick path, why actions go
// through Cbuf_ExecuteText, why input is never rotated).

#include <SDL.h>
#include <math.h>

#include "qcommon/qcommon.h"
#include "qcommon/q_shared.h"
#include "client/client.h"
#include "sys/sys_local.h"
#include "sdl_gamepad.h"

#ifdef AURORA

// ---------------------------------------------------------------------------
// cvars
// ---------------------------------------------------------------------------

static cvar_t *cl_gamepad;					// 0 off, 1 on (default) - master switch
static cvar_t *cl_gamepadDeadZone;			// 0..1 fraction of stick travel ignored, both sticks
static cvar_t *cl_gamepadTriggerThreshold;	// 0..1 fraction of trigger travel counted as "pressed"
static cvar_t *cl_gamepadLookSpeed;		// mouse units/sec at full right-stick deflection

static SDL_GameController *gamepad = NULL;
static SDL_JoystickID gamepadInstance = -1;

// ---------------------------------------------------------------------------
// state gates - same two switches sdl_touchui.cpp already uses to tell
// menu from gameplay (Key_GetCatcher() & KEYCATCH_UI, cls.state).
// ---------------------------------------------------------------------------

static int Aurora_Gamepad_InGameplay( void )
{
	return cls.state == CA_ACTIVE && !( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) );
}

static int Aurora_Gamepad_MenuActive( void )
{
	return ( Key_GetCatcher() & KEYCATCH_UI ) != 0;
}

// Right stick doubles as the camera in gameplay and the cursor in the
// menu - active a little more broadly than Aurora_Gamepad_InGameplay()
// (e.g. also with the console down), exactly like a real mouse still
// turns the camera with the console down.
static int Aurora_Gamepad_RightStickActive( void )
{
	if ( Aurora_Gamepad_MenuActive() ) return 1;
	return cls.state == CA_ACTIVE;
}

// ---------------------------------------------------------------------------
// held actions (buttons/triggers/stick that map to +action/-action) - one
// flag per action, so leaving gameplay (menu opens, disconnect, cl_gamepad
// turned off) can cleanly release everything still held, exactly like
// sdl_touchui.cpp's ReleaseButton/StickStop cleanup.
// ---------------------------------------------------------------------------

static int heldForward, heldBack, heldMoveLeft, heldMoveRight;
static int heldJump, heldCrouch, heldUse, heldFire, heldAltFire;

static void Aurora_Gamepad_ReleaseAll( void )
{
	if ( heldForward )   { Cbuf_ExecuteText( EXEC_APPEND, "-forward\n" );   heldForward = 0; }
	if ( heldBack )      { Cbuf_ExecuteText( EXEC_APPEND, "-back\n" );      heldBack = 0; }
	if ( heldMoveLeft )  { Cbuf_ExecuteText( EXEC_APPEND, "-moveleft\n" );  heldMoveLeft = 0; }
	if ( heldMoveRight ) { Cbuf_ExecuteText( EXEC_APPEND, "-moveright\n" ); heldMoveRight = 0; }
	if ( heldJump )      { Cbuf_ExecuteText( EXEC_APPEND, "-moveup\n" );    heldJump = 0; }
	if ( heldCrouch )    { Cbuf_ExecuteText( EXEC_APPEND, "-movedown\n" );  heldCrouch = 0; }
	if ( heldUse )       { Cbuf_ExecuteText( EXEC_APPEND, "-use\n" );       heldUse = 0; }
	if ( heldFire )      { Cbuf_ExecuteText( EXEC_APPEND, "-attack\n" );    heldFire = 0; }
	if ( heldAltFire )   { Cbuf_ExecuteText( EXEC_APPEND, "-altattack\n" ); heldAltFire = 0; }
}

// ---------------------------------------------------------------------------
// left stick -> movement (gameplay only, digital emulation of the four
// move actions - same threshold-crossing scheme as sdl_touchui.cpp's
// Aurora_TouchUI_StickApply, just fed by SDL_CONTROLLER_AXIS_LEFTX/LEFTY
// instead of a synthesized finger vector).
// ---------------------------------------------------------------------------

static void Aurora_Gamepad_LeftStick( int gameplay )
{
	float lx, ly, side, forward, dz;
	int forwardNow, backNow, leftNow, rightNow;

	if ( !gameplay )
	{
		if ( heldForward || heldBack || heldMoveLeft || heldMoveRight )
		{
			if ( heldForward )   { Cbuf_ExecuteText( EXEC_APPEND, "-forward\n" );   heldForward = 0; }
			if ( heldBack )      { Cbuf_ExecuteText( EXEC_APPEND, "-back\n" );      heldBack = 0; }
			if ( heldMoveLeft )  { Cbuf_ExecuteText( EXEC_APPEND, "-moveleft\n" );  heldMoveLeft = 0; }
			if ( heldMoveRight ) { Cbuf_ExecuteText( EXEC_APPEND, "-moveright\n" ); heldMoveRight = 0; }
		}
		return;
	}

	lx = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_LEFTX ) / 32767.0f;
	ly = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_LEFTY ) / 32767.0f;
	dz = cl_gamepadDeadZone->value;

	side = lx;
	forward = -ly;	// SDL's Y axis is positive-down; pushing the stick up is forward

	forwardNow = forward >  dz;
	backNow    = forward < -dz;
	leftNow    = side    < -dz;
	rightNow   = side    >  dz;

	if ( forwardNow != heldForward )   { Cbuf_ExecuteText( EXEC_APPEND, forwardNow ? "+forward\n"   : "-forward\n" );   heldForward = forwardNow; }
	if ( backNow    != heldBack )      { Cbuf_ExecuteText( EXEC_APPEND, backNow    ? "+back\n"       : "-back\n" );      heldBack = backNow; }
	if ( leftNow    != heldMoveLeft )  { Cbuf_ExecuteText( EXEC_APPEND, leftNow    ? "+moveleft\n"   : "-moveleft\n" );  heldMoveLeft = leftNow; }
	if ( rightNow   != heldMoveRight ) { Cbuf_ExecuteText( EXEC_APPEND, rightNow   ? "+moveright\n"  : "-moveright\n" ); heldMoveRight = rightNow; }
}

// ---------------------------------------------------------------------------
// right stick -> camera look (gameplay) / cursor (menu). Synthesizes the
// same SE_MOUSE deltas a real SDL_MOUSEMOTION would produce - NOT passed
// through Aurora_TransformInputDelta*, per gameport/AGENTS.MD stage 4
// ("Ввод от геймпада... НЕ трансформируем"). Rate-based (scaled by frame
// time), unlike the touch camera pad which is driven by finger deltas -
// there is no finger here, only a continuous deflection to integrate.
// ---------------------------------------------------------------------------

static float lookRemX, lookRemY;

static void Aurora_Gamepad_RightStick( void )
{
	float rx, ry, dz, mag;
	float unitsPerSec, dxf, dyf;
	int dx, dy;

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

	// Rescale so the response starts at 0 right past the dead zone
	// instead of jumping straight to (mag - dz)'s value.
	{
		float scale = ( mag - dz ) / ( 1.0f - dz ) / mag;
		rx *= scale;
		ry *= scale;
	}

	unitsPerSec = cl_gamepadLookSpeed->value;
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
// triggers -> fire / alt-fire (gameplay only). No native "digital trigger"
// event exists - edge-detected against cl_gamepadTriggerThreshold, exactly
// like the old joystick path thresholds an axis (sdl_input.cpp's
// in_joystickThreshold), just mapped to the real +attack/-altattack
// actions instead of a synthesized key.
// ---------------------------------------------------------------------------

static void Aurora_Gamepad_Triggers( int gameplay )
{
	float rt, lt, threshold;
	int fireNow, altFireNow;

	if ( !gameplay )
	{
		if ( heldFire )    { Cbuf_ExecuteText( EXEC_APPEND, "-attack\n" );    heldFire = 0; }
		if ( heldAltFire ) { Cbuf_ExecuteText( EXEC_APPEND, "-altattack\n" ); heldAltFire = 0; }
		return;
	}

	threshold = cl_gamepadTriggerThreshold->value;
	rt = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT ) / 32767.0f;
	lt = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT ) / 32767.0f;

	fireNow    = rt > threshold;
	altFireNow = lt > threshold;

	if ( fireNow    != heldFire )    { Cbuf_ExecuteText( EXEC_APPEND, fireNow    ? "+attack\n"    : "-attack\n" );    heldFire = fireNow; }
	if ( altFireNow != heldAltFire ) { Cbuf_ExecuteText( EXEC_APPEND, altFireNow ? "+altattack\n"  : "-altattack\n" ); heldAltFire = altFireNow; }
}

// ---------------------------------------------------------------------------
// A: jump in gameplay, click in the menu. B: crouch. X: use. All three
// are plain digital buttons via SDL_GameControllerGetButton.
// ---------------------------------------------------------------------------

static int wasA, wasB, wasX;

static void Aurora_Gamepad_FaceButtons( int gameplay, int menu )
{
	int a = SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_A ) != 0;
	int b = SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_B ) != 0;
	int x = SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_X ) != 0;

	if ( gameplay )
	{
		if ( a != heldJump ) { Cbuf_ExecuteText( EXEC_APPEND, a ? "+moveup\n"   : "-moveup\n" );   heldJump   = a; }
		if ( b != heldCrouch ){ Cbuf_ExecuteText( EXEC_APPEND, b ? "+movedown\n" : "-movedown\n" ); heldCrouch = b; }
		if ( x != heldUse )   { Cbuf_ExecuteText( EXEC_APPEND, x ? "+use\n"      : "-use\n" );      heldUse    = x; }
	}
	else
	{
		if ( heldJump )   { Cbuf_ExecuteText( EXEC_APPEND, "-moveup\n" );   heldJump = 0; }
		if ( heldCrouch ) { Cbuf_ExecuteText( EXEC_APPEND, "-movedown\n" ); heldCrouch = 0; }
		if ( heldUse )    { Cbuf_ExecuteText( EXEC_APPEND, "-use\n" );      heldUse = 0; }

		// A clicks in the menu - a tap, on the press edge, exactly like the
		// touch-UI trackpad's tap-to-click (sdl_input.cpp's aurora_touch
		// SDL_FINGERUP handling).
		if ( menu && a && !wasA )
		{
			Sys_QueEvent( 0, SE_KEY, A_MOUSE1, qtrue, 0, NULL );
			Sys_QueEvent( 0, SE_KEY, A_MOUSE1, qfalse, 0, NULL );
		}
	}

	wasA = a;
	wasB = b;
	wasX = x;
}

// ---------------------------------------------------------------------------
// LB/RB -> previous/next weapon; D-pad -> four fixed weapon slots
// (code/game/weapons.h: WP_SABER=1, WP_BLASTER_PISTOL=2, WP_BLASTER=3,
// WP_THERMAL=9 - saber, sidearm, primary blaster, thermal detonator: a
// spread that is useful on its own, distinct from LB/RB's plain cycling).
// One-shot commands (no held state), fired on the press edge only,
// gameplay only.
// ---------------------------------------------------------------------------

typedef struct {
	SDL_GameControllerButton	button;
	const char					*cmd;
	int							wasDown;
} gamepadOneShot_t;

static gamepadOneShot_t oneShots[] = {
	{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  "weapprev\n", 0 },
	{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "weapnext\n", 0 },
	{ SDL_CONTROLLER_BUTTON_DPAD_UP,       "weapon 1\n", 0 },	// WP_SABER
	{ SDL_CONTROLLER_BUTTON_DPAD_LEFT,     "weapon 2\n", 0 },	// WP_BLASTER_PISTOL
	{ SDL_CONTROLLER_BUTTON_DPAD_DOWN,     "weapon 3\n", 0 },	// WP_BLASTER
	{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    "weapon 9\n", 0 },	// WP_THERMAL
};
static const size_t numOneShots = sizeof( oneShots ) / sizeof( oneShots[0] );

static void Aurora_Gamepad_OneShots( int gameplay )
{
	size_t i;

	for ( i = 0; i < numOneShots; i++ )
	{
		int down = gameplay && SDL_GameControllerGetButton( gamepad, oneShots[i].button ) != 0;

		if ( down && !oneShots[i].wasDown )
		{
			Cbuf_ExecuteText( EXEC_APPEND, oneShots[i].cmd );
		}
		oneShots[i].wasDown = down;
	}
}

// ---------------------------------------------------------------------------
// Start -> Escape (pause/menu toggle or back-out), available in every
// state - not gated to gameplay at all, mirroring the touch-UI's menu
// button (sdl_touchui.cpp's TB_MENU, downCmd == NULL: sends A_ESCAPE
// straight into the event queue instead of a console command, since
// Escape's meaning is entirely context-sensitive inside the engine
// already, see research/touch_ui_research.md п.2/4).
// ---------------------------------------------------------------------------

static int wasStart;

static void Aurora_Gamepad_Start( void )
{
	int down = SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_START ) != 0;

	if ( down && !wasStart )
	{
		Sys_QueEvent( 0, SE_KEY, A_ESCAPE, qtrue, 0, NULL );
		Sys_QueEvent( 0, SE_KEY, A_ESCAPE, qfalse, 0, NULL );
	}
	wasStart = down;
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

	Aurora_Gamepad_ReleaseAll();
	SDL_GameControllerClose( gamepad );
	gamepad = NULL;
	gamepadInstance = -1;
	wasA = wasB = wasX = wasStart = 0;
	lookRemX = lookRemY = 0.0f;
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
	int gameplay, menu;

	if ( !cl_gamepad->integer || !gamepad || !SDL_GameControllerGetAttached( gamepad ) )
	{
		Aurora_Gamepad_ReleaseAll();
		return;
	}

	gameplay = Aurora_Gamepad_InGameplay();
	menu = Aurora_Gamepad_MenuActive();

	Aurora_Gamepad_LeftStick( gameplay );
	Aurora_Gamepad_RightStick();
	Aurora_Gamepad_Triggers( gameplay );
	Aurora_Gamepad_FaceButtons( gameplay, menu );
	Aurora_Gamepad_OneShots( gameplay );
	Aurora_Gamepad_Start();
}

void Aurora_Gamepad_Init( void )
{
	cl_gamepad = Cvar_Get( "cl_gamepad", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepad, 0.0f, 1.0f, qtrue );
	cl_gamepadDeadZone = Cvar_Get( "cl_gamepadDeadZone", "0.22", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepadDeadZone, 0.02f, 0.9f, qfalse );
	cl_gamepadTriggerThreshold = Cvar_Get( "cl_gamepadTriggerThreshold", "0.25", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepadTriggerThreshold, 0.05f, 0.95f, qfalse );
	cl_gamepadLookSpeed = Cvar_Get( "cl_gamepadLookSpeed", "250", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_gamepadLookSpeed, 10.0f, 4000.0f, qfalse );

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

#endif // AURORA
