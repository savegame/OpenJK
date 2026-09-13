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

// Aurora gamepad support - rebindable dual-stick layout for a physical
// USB/Bluetooth gamepad. Built on SDL_GameController (SDL_gamecontroller.h),
// not the legacy SDL_Joystick path already in this file's neighbour
// sdl_input.cpp (in_joystick/in_joystickNo/in_joystickUseAnalog,
// CL_JoystickMove in cl_input.cpp): that old path exposes raw, unnamed
// axes/buttons and is built around a single analog stick doing double duty
// for yaw/pitch AND forward move (gated by in_strafe/in_mlooking) - it
// cannot express two independent sticks (move + look) at once, which this
// layout needs, and CL_JoystickMove wires its axes straight to
// forward/side/up move with no bind table in between (not rebindable). The
// legacy path is left completely untouched for players who still rely on
// it with an exotic controller SDL cannot map. SDL_GameController gives
// named buttons (A/B/X/Y, LB/RB, LT/RT, D-pad, ...) and two independent
// sticks out of the box for any controller SDL's bundled mapping DB
// recognises (essentially all Xbox/PlayStation-style pads, wired or
// Bluetooth).
//
// Rebinding, end to end
// ----------------------
// Every physical button (and, for the left stick and triggers, every
// digital direction/pull synthesised from an analog axis) is delivered to
// the engine as a normal SE_KEY event carrying one of the dedicated
// joystick keycodes A_JOY0..A_JOY31 (code/client/keycodes.h) - the exact
// same code path and key namespace Quake3-derived engines have always used
// for joystick buttons (keynames "JOY0".."JOY31" in code/client/cl_keys.cpp
// already resolve these to bindable actions). Concretely:
// Sys_QueEvent( 0, SE_KEY, A_JOY0 + n, down, 0, NULL ). This module NEVER
// emulates a keyboard keycode (A_SPACE, A_W, ...) and NEVER calls
// Cbuf_ExecuteText to fire "+action" directly - doing either would collide
// with the player's real keyboard bindings or hardcode the layout in C++.
// Because JOYn events go through the engine's normal binding path
// (CL_ParseBinding, cl_keys.cpp), `bind JOY0 +attack` from the in-game
// Controls menu or config just works, and it composes for free with the
// engine's own catcher-aware suppression: a "+action" bind only fires while
// no menu/console is catching keys (Key_GetCatcher() == 0), exactly like a
// real keyboard/joystick key - so this module does not need to track
// "gameplay vs. menu" itself for ordinary buttons, only for the right
// stick's dual role (see below).
//
// JOYn assignment (stable - configs and this module both depend on it):
// JOY0..JOY20  = the SDL_GameControllerButton enum value directly
//                (A=0, B=1, X=2, Y=3, BACK=4, GUIDE=5, START=6,
//                LEFTSTICK=7, RIGHTSTICK=8, LEFTSHOULDER=9,
//                RIGHTSHOULDER=10, DPAD_UP=11, DPAD_DOWN=12, DPAD_LEFT=13,
//                DPAD_RIGHT=14, MISC1=15, PADDLE1..4=16..19, TOUCHPAD=20).
//                SDL only ever appends to this enum, so the mapping is
//                stable across SDL versions.
// JOY21        = left trigger (LT), digitised: down when the axis exceeds
//                cl_gamepadTriggerThreshold.
// JOY22        = right trigger (RT), same digitisation.
// JOY23..26    = left stick, ALSO exposed as four digital direction slots
//                (up/down/left/right), down when the axis exceeds
//                cl_gamepadDeadZone - kept only as an optional, fully
//                rebindable fallback (e.g. "bind JOY23 +forward" for a
//                player who wants dpad-style digital movement back, or
//                wants to put something else entirely on these slots).
//                They are NOT bound by default any more (see below) and
//                are NOT how the left stick drives movement day to day -
//                that is Aurora_Gamepad_GetMove()'s job (analog, see
//                below), read directly by CL_AuroraGamepadMove()
//                (code/client/cl_input.cpp) once per usercmd. Movement
//                needed a genuinely continuous, variable-speed response
//                (walk at partial deflection, smoothly up to full run at
//                full deflection) - a purely digital JOYn/bind path can
//                only ever be "pressed" or "not pressed", i.e. dpad
//                emulation, which is exactly what this task replaced.
//                Analog movement still deliberately bypasses the engine's
//                other analog-axis path (SE_JOYSTICK_AXIS ->
//                CL_JoystickEvent -> CL_JoystickMove, qcommon/common.cpp +
//                cl_input.cpp): that legacy single-stick path ties the
//                side axis to camera yaw unless in_strafe is held, and the
//                forward axis to pitch look while in_mlooking, because it
//                was designed for a controller with only one analog stick
//                doing double duty for look AND move - wrong on this
//                dual-stick layout, where the right stick already owns
//                look via SE_MOUSE (see below) and the left stick should
//                always mean "move", never "turn". Aurora_Gamepad_GetMove()
//                instead hands CL_AuroraGamepadMove() ready-to-clamp
//                forwardmove/rightmove deltas (plus a walk/run verdict for
//                BUTTON_WALKING) that go straight into the usercmd, the
//                same -127..127 continuum a keyboard's Run key already
//                produces at its two fixed points (64/127) - the stick
//                just fills in everything in between.
// JOY27..31    = unused, reserved.
//
// Right stick is the one deliberate exception: it is delivered as SE_MOUSE
// deltas (CL_MouseEvent), exactly as a real mouse would produce them, and
// is NOT bindable - camera-look and menu-cursor movement are properties of
// the mouse input path itself in this engine (sensitivity/curve cvars, not
// a `bind`-table action), so expressing them as "phantom mouse movement"
// is the natural fit, matches how a real Xbox/PS controller's right stick
// works in effectively every other Quake3-derived port, and keeps the
// existing cl_gamepad* sensitivity cvars meaningful. Two independent
// speeds/cvars are used because the two consumers of that SE_MOUSE stream
// behave very differently downstream: in the menu it drives the cursor
// directly (_UI_MouseEvent, no extra scaling), while in gameplay it goes
// through CL_MouseMove and is further scaled by cl_sensitivity/m_yaw/
// m_pitch/FOV - so a single shared speed cannot feel right in both places
// at once (see cvar list below).
//
// Per gameport/AGENTS.MD, stage 4 "Трансформация ввода": gamepad input is
// NEVER run through the screen-rotation transform
// (Aurora_TransformInputDelta*) - unlike mouse/touch, it does not care
// about screen orientation. Every event this module synthesises is queued
// exactly as a real device would produce it, with no rotation/scale
// applied.
//
// A is the one button with a second, non-rebindable role: while a menu is
// showing (Key_GetCatcher() & KEYCATCH_UI), on the press edge it also
// injects a synthetic A_MOUSE1 click (press+release), exactly like the
// touch-UI trackpad's tap-to-click - this is standing in for "the mouse
// button the UI already knows how to react to", not a keyboard keycode, so
// it does not conflict with rebinding; JOY0 itself is still sent as an
// ordinary bindable button underneath (bound to +moveup/jump by default).
//
// Default bindings ship as a config, not code (see
// aurora_gamepad_defaults.cfg, installed by rpm/openjk.spec and executed
// by qcommon/common.cpp's Com_ExecuteCfg after default.cfg but before the
// player's own config/autoexec, so a rebind always survives the next
// launch) - this module only ever emits JOYn/SE_MOUSE events, it does not
// itself decide what any button "does".
//
// Sensitivity/behaviour cvars (unaffected by this task - these are feel,
// not layout, and keep their AGENTS.md-mandated names):
//   cl_gamepad               - 0 off, 1 on (master switch)
//   cl_gamepadDeadZone        - both sticks' dead zone, and the left
//                               stick's digitisation threshold
//   cl_gamepadTriggerThreshold- LT/RT digitisation threshold
//   cl_gamepadLookSpeed       - right stick -> camera turn rate in
//                               gameplay (mouse units/sec at full
//                               deflection, pre-cl_sensitivity/m_yaw)
//   cl_gamepadMenuSpeed       - right stick -> menu cursor speed
//                               (independent of cl_gamepadLookSpeed - see
//                               rationale above)
//   cl_gamepadLookCurve       - 0 linear, 1 (default) quadratic response
//                               on the gameplay look axis only (precise
//                               aim near center, fast turns at full
//                               deflection, same top speed either way);
//                               the menu cursor is always linear.
//   cl_gamepadMoveCurve       - 0 linear, 1 (default) quadratic response
//                               on the left stick's analog move magnitude
//                               (see Aurora_Gamepad_GetMove() below) -
//                               same idea as cl_gamepadLookCurve: easier
//                               to hold a precise slow walk near center,
//                               same full run at full deflection either
//                               way.
//
// The existing physical-input-displaces-touch-ui rule
// (Aurora_TouchUI_NoteJoystickChange/Aurora_TouchUI_RealJoystickConnected
// in sdl_touchui.cpp) already reacts to any real USB/Bluetooth joystick
// via SDL_JOYDEVICEADDED/REMOVED - SDL_GameControllerOpen opens the same
// underlying joystick device, so that hide/show logic needs no changes
// here and keeps working unmodified for a connected gamepad.

union SDL_Event;

// IN_Init/IN_Shutdown (sdl_input.cpp).
void Aurora_Gamepad_Init( void );
void Aurora_Gamepad_Shutdown( void );

// Once a frame, from IN_Frame() - polls sticks/triggers/buttons and
// dispatches the actions described above. Polling (not event-driven)
// keeps continuous analog state (stick deflection, trigger pull) and the
// digital button edge-detection in one simple place, exactly like
// sdl_touchui.cpp's Aurora_TouchUI_Frame/StickApply.
void Aurora_Gamepad_Frame( void );

// SDL_CONTROLLERDEVICEADDED / SDL_CONTROLLERDEVICEREMOVED - opens the
// first recognised controller / closes it and tries the next one still
// plugged in. Called from IN_ProcessEvents's event switch.
void Aurora_Gamepad_Event( const SDL_Event *ev );

// Left stick, continuous - called once per usercmd from
// CL_AuroraGamepadMove() (code/client/cl_input.cpp), NOT from anywhere in
// this file's own per-frame polling. See the JOY23..26 section above for
// why the left stick is analog rather than routed through JOY binds or
// through the engine's legacy CL_JoystickMove path.
//
// Returns qfalse (leaving *forwardmove/*rightmove/*walking untouched) when
// the gamepad is off/disconnected or the stick is inside the dead zone -
// so the caller only ever touches cmd/BUTTON_WALKING when the stick
// actually wants to say something, never fighting a keyboard-only frame.
//
// On qtrue:
//   *forwardmove/*rightmove - -127..127, dead-zone-rescaled and (per
//                              cl_gamepadMoveCurve) curve-shaped, ready to
//                              add onto usercmd_t's own fields the same
//                              way CL_JoystickMove adds cl.joystickAxis[].
//   *walking                - qtrue below half deflection (mirrors
//                              CL_KeyMove's own walk speed of 64 out of a
//                              127 max), qfalse from there up to full
//                              deflection - so a full push always yields
//                              the same BUTTON_WALKING-clear, full-127
//                              magnitude a held Run key gives today,
//                              never a value in between.
qboolean Aurora_Gamepad_GetMove( int *forwardmove, int *rightmove, qboolean *walking );
