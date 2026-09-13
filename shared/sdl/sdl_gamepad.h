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

// Aurora gamepad support - a default dual-stick layout for a physical
// USB/Bluetooth gamepad (task: "Подключить геймпад... и задать дефолтную
// раскладку"). Built on SDL_GameController (SDL_gamecontroller.h), not the
// legacy SDL_Joystick path already in this file's neighbour sdl_input.cpp
// (in_joystick/in_joystickNo/in_joystickUseAnalog, CL_JoystickMove in
// cl_input.cpp): that old path exposes raw, unnamed axes/buttons and is
// built around a single analog stick doing double duty for yaw/pitch AND
// forward move (gated by in_strafe/in_mlooking) - it cannot express two
// independent sticks (move + look) at once, which this layout needs. The
// legacy path is left completely untouched for players who still rely on
// it with an exotic controller SDL cannot map. SDL_GameController gives
// named buttons (A/B/X/Y, LB/RB, LT/RT, D-pad) and two independent sticks
// out of the box for any controller SDL's bundled mapping DB recognises
// (essentially all Xbox/PlayStation-style pads, wired or Bluetooth).
//
// Per gameport/AGENTS.MD, stage 4 "Трансформация ввода": gamepad input is
// NEVER run through the screen-rotation transform
// (Aurora_TransformInputDelta*) - unlike mouse/touch, it does not care
// about screen orientation. Every event this module synthesises
// (SE_MOUSE for the right stick, SE_KEY for Start/Escape) is queued
// exactly as a real device would produce it, with no rotation/scale
// applied.
//
// Actions are dispatched exactly like the touch-UI overlay
// (research/touch_ui_research.md п.2, sdl_touchui.cpp): held actions go
// through Cbuf_ExecuteText(EXEC_APPEND, "+action\n"/"-action\n"), taps
// through one-shot console commands (weapnext/weapprev/weapon N), and the
// Start button injects Sys_QueEvent(SE_KEY, A_ESCAPE, ...) the same way
// the touch-UI's menu button does - never key emulation, so the layout
// keeps working even if the player rebinds +attack etc. in the options
// menu.
//
// Left stick = move (gameplay only); right stick = camera look in
// gameplay, mouse cursor in the menu (Key_GetCatcher() & KEYCATCH_UI,
// same gate sdl_touchui.cpp already uses to tell menu from gameplay); A =
// jump in gameplay, click in the menu; B = crouch; X = use; RT = attack;
// LT = alt-attack; LB/RB = previous/next weapon; D-pad = four fixed
// weapon slots (saber/pistol/blaster/thermal detonator); Start = Escape
// (pause/back), available in every state.
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
