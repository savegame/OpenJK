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
// gameport/docs/touch_ui.md) - the gameplay-only virtual gamepad overlay:
// a floating left-half stick, a right-half camera pad (emulates the mouse)
// and a handful of action buttons that call the engine's real +action
// commands (never key emulation - see research/touch_ui_research.md п.2).
// Menu/console touch is untouched, separate code (the "aurora_touch"
// trackpad already in sdl_input.cpp, port stage 4).
//
// Implemented in sdl_touchui.cpp, a module of its own: it owns its layout
// (millimetres of physical DPI -> window px), its own per-fingerId state
// machines (stick/pad/buttons never share a finger) and its own cvars.
// sdl_input.cpp only routes SDL events into it and calls it once a frame.

union SDL_Event;

// IN_Init/IN_Shutdown (sdl_input.cpp)
void Aurora_TouchUI_Init( void );
void Aurora_TouchUI_Shutdown( void );

// Once a frame, from IN_Frame() - refreshes the layout, applies the
// physical-input-displaces-touch-ui rule and ships the current overlay to
// the renderer (re.Aurora_SetTouchOverlay, tr_public.h).
void Aurora_TouchUI_Frame( void );

// SDL_FINGERDOWN/MOTION/UP, gameplay only - the caller (IN_ProcessEvents)
// already excludes the KEYCATCH_UI (menu) case, which stays on the existing
// trackpad code path.
void Aurora_TouchUI_FingerEvent( const SDL_Event *ev );

// A genuine physical mouse/keyboard event was just processed - hide the
// overlay until the screen is touched again. Safe to call every such event;
// cheap (one flag write).
void Aurora_TouchUI_NotePhysicalInput( void );

// SDL_JOYDEVICEADDED / SDL_JOYDEVICEREMOVED - re-evaluates whether a
// gamepad is currently connected.
void Aurora_TouchUI_NoteJoystickChange( void );

// ---------------------------------------------------------------------------
// Implemented in sdl_input.cpp, used by sdl_touchui.cpp: both are compiled
// into the same client binary (no dlopen boundary here, unlike the
// renderer), but sdl_touchui.cpp keeps its own translation unit, so these
// two small accessors are the entire surface it needs back.
// ---------------------------------------------------------------------------

// Live cache of the current wl_output_transform (WL_OUTPUT_TRANSFORM_*,
// see sdl_input.cpp's Aurora_ApplyOrientation) - the same value
// Aurora_TransformInputDeltaF already inverts for real mouse/menu-trackpad
// deltas. The touch-ui module needs it too, to convert absolute touch
// points and to synthesise its own camera-pad mouse deltas through the
// identical rotation table (see sdl_touchui.cpp's derivation comment).
int Aurora_TouchUI_GetTransform( void );

// The SDL window handle IN_Init was given - sdl_input.cpp and
// sdl_window.cpp each keep their own static copy already; this is the
// touch-ui module's only need for it (SDL_GetWindowSize/SDL_GetDisplayDPI).
// Callers must #include <SDL.h> before this header (both sdl_input.cpp and
// sdl_touchui.cpp already do, for SDL_Event/SDL_Window itself).
struct SDL_Window *Aurora_TouchUI_GetWindow( void );
