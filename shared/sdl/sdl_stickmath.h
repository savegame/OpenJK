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

// Aurora analog-stick math - the radial dead zone + optional quadratic
// response curve shared by every "-1..1 stick vector in, -1..1 rescaled
// vector + 0..1 magnitude out" analog move source in this port: the
// physical gamepad's left stick (sdl_gamepad.cpp) and the touch-UI's
// virtual stick (sdl_touchui.cpp) both reduce to exactly this shape once
// their own raw input (SDL_GameControllerAxis fraction; finger distance
// from the stick's centre, normalized by its travel radius) is expressed
// as a -1..1 vector where length 1.0 == full deflection - see each
// caller for how it gets there. Kept here, header-only, so the two
// independent modules (different translation units, neither includes the
// other) share one definition of "how a stick's dead zone and curve
// work" instead of two copies that could quietly drift apart.
//
// The dead zone is applied radially (by vector length), not per-axis, so
// a diagonal push is not shortchanged by a square dead zone - see
// Aurora_Gamepad_LeftStick's own comment in sdl_gamepad.cpp for the
// original rationale.

#include <math.h>

#include "qcommon/q_shared.h"

// x, y: raw stick vector, already scaled so length 1.0 == full deflection
//       (SDL axis fraction for the gamepad; finger-offset-from-centre /
//       stick-radius-mm for the touch UI).
// dz:   0..1, fraction of full deflection to treat as dead zone.
// curve: qtrue applies a quadratic response (precise near centre, same
//        full speed at full deflection either way); qfalse is linear.
//
// Returns qfalse (and zeroes *outX/*outY/*outMag) when the vector's
// length is inside the dead zone - callers use this to mean "the stick
// isn't saying anything this update", exactly like
// Aurora_Gamepad_GetMove's own moveActive flag.
//
// On qtrue, *outX/*outY are the direction-preserving rescale of x,y so
// response starts at 0 right past the dead zone rather than jumping
// straight to (mag - dz)'s raw value, and *outMag is that same rescale's
// magnitude (0..1, direction-independent) - the "how far past the dead
// zone" value each caller uses for its own walk/run decision
// (AURORA_STICK_RUN_FRACTION below).
static inline qboolean Aurora_Stick_Radial( float x, float y, float dz, qboolean curve,
	float *outX, float *outY, float *outMag )
{
	float mag = sqrtf( x * x + y * y );
	float t;

	if ( mag < dz || mag < 0.0001f )
	{
		*outX = *outY = *outMag = 0.0f;
		return qfalse;
	}

	t = ( mag - dz ) / ( 1.0f - dz );
	if ( t > 1.0f )
	{
		t = 1.0f;
	}

	if ( curve )
	{
		// Quadratic response: easier to hold a slow, precise walk near the
		// dead zone, same full run at t==1 either way.
		t = t * t;
	}

	*outX = x * ( t / mag );
	*outY = y * ( t / mag );
	*outMag = t;
	return qtrue;
}

// Mirrors CL_KeyMove's own walk speed of 64 out of a 127 max - below this
// fraction of full deflection an analog stick should still read as
// "walking" for BUTTON_WALKING (leg anim/footstep volume, bg_pmove.cpp),
// exactly the speed a keyboard's Shift-less forward key already produces;
// from there up to full deflection it reads as running, the same way
// holding Shift (or cl_run 1) does today. Shared by the gamepad's left
// stick and the touch-UI's virtual stick so both analog move sources use
// the exact same walk/run split. Deliberately a fixed fraction, not a
// cvar - it is what "the same speeds a keyboard already uses" means, not
// a feel knob.
#define AURORA_STICK_RUN_FRACTION ( 64.0f / 127.0f )
