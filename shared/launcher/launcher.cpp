/*
launcher.cpp - AuroraOS in-process ImGui launcher for OpenJK (Jedi Academy)
Copyright (C) 2026

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Scope (gameport/docs/imgui_launcher.md, gameport/examples/launcher/ is the
reference this is adapted from):
  * Create an SDL window + GLES3 context with the same attributes the
    engine will later expect (see shared/sdl/sdl_window.cpp GLimp_SetMode -
    RGBA 8/8/8/8, DEPTH 24, STENCIL 8, doublebuffer, GL ES 3.0).
  * Initialise ImGui (SDL2 platform + OpenGL3 renderer, ES3 mode), apply
    the port's theme (theme.cpp, palette from docs/imgui-theme-spec.md).
  * Tabs: "Игра" (resource picker + start button), "Настройки" (FBO
    scale), "О программе" (disclaimer + links).
  * Touch: SDL on AuroraOS sends finger events (tfinger, normalised 0..1),
    not mouse events (mouse-from-touch synthesis is disabled by a hint in
    Launcher_Run). A tap (<16 px of travel) becomes a mouse down+up pair;
    a drag becomes MouseWheel pulses so ImGui lists scroll 1:1 under the
    finger.
  * On Start: shut down ImGui (releases its own VAO/VBO/program/font
    texture) but leave the SDL window and GL context alive. The engine's
    window-creation code (shared/sdl/sdl_window.cpp) sees
    Launcher_GetWindow/GLContext non-NULL and skips its own creation
    calls - critical on AuroraOS where the compositor terminates apps
    whose window disappears even briefly.
  * The picked resource directory and FBO scale are NOT handed to the
    engine via environment variables: fs_basepath is a CVAR_INIT cvar
    (code/qcommon/files.cpp) that can only be set from the command line,
    before Com_Init runs - exactly how device.md documents a manual
    launch ("+set fs_basepath ... +map academy1"). sys_main.cpp reads
    them back via Launcher_GetResourceDir()/Launcher_GetFboScale() after
    Launcher_Run() returns and prepends the equivalent "+set" tokens to
    the real command line before calling Com_Init.
*/

#include "launcher.h"
#include "theme.h"

#include <SDL.h>
#include <SDL_video.h>
#include <SDL_misc.h> /* SDL_OpenURL */

#include <GLES3/gl3.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

/* Font embedded into the package: Noto Sans Bold (Cyrillic), stb-compressed
   array. Regenerate with binary_to_compressed_c from the TTF if needed. */
#include "fonts/NotoSansBold.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <pwd.h>

#include <string>
#include <vector>
#include <algorithm>

/* Sandbox identifiers - build system defines (code/CMakeLists.txt via
   AuroraDefines, openjk/CMakeLists.txt:339). Must match port_config.json,
   the RPM spec (%{_app_orgname}.%{_app_appname}) and the paths
   shared/sys/sys_unix.cpp uses for fs_homepath. */
#ifndef AURORA_ORG
#define AURORA_ORG "org.example"
#endif
#ifndef AURORA_APP
#define AURORA_APP "mygame"
#endif

/* Display name only - AURORA_APP above stays the filesystem/package id
   ("openjk"), this is what the player actually sees. */
#define GAME_TITLE "Jedi Academy"

namespace {

SDL_Window    *g_window  = nullptr;
SDL_GLContext  g_context = nullptr;
bool           g_owned   = false; // true while launcher still owns the window/context

/* Launcher widgets are 20% more compact than the font scale: at a
   comfortable font size the default widget metrics looked oversized. All
   explicit widget sizes are derived from ES() instead of
   ImGui::GetFontSize() (the font itself is not scaled), and the theme
   metrics are additionally multiplied by kElemScale after ApplyTheme
   (see Launcher_Run). */
static const float kElemScale = 0.8f;
static inline float ES() { return ImGui::GetFontSize() * kElemScale; }

/* Extra vertical compression (font untouched): buttons 20% lower,
   tabs 30% lower. */
static const float kBtnScale = 0.8f;
static const float kTabScale = 0.7f;

// ----------------------------------------------------------------------
// Resource picker state and launcher result
// ----------------------------------------------------------------------
struct PickerState
{
	std::string current_dir; // directory currently shown in the browser
	std::string selected;    // last confirmed pick (the resource root)
	bool        browser_open = false;
	bool        valid_pick   = false; // selected points at a valid resource root
};

PickerState g_picker;

/* Deferred directory-browser open: the "Browse..." button fires on the
   tap release, and the browser window is drawn later in the same frame -
   without the deferral the same release would "fall through" into the
   just-opened window and click whatever sits under the finger. The flag is
   set on click, the open happens at the start of the next frame
   (DrawLauncherUI). */
bool g_browser_open_pending = false;

/* Result: has the user asked to start the game. */
bool g_launch = false;

/* Touch: tap vs drag. Finger motion is NOT translated into mouse motion
   (buttons would drag-select): a drag becomes MouseWheel pulses to the
   hovered ImGui window, a tap is a single down+up at the touch point. */
struct TouchState
{
	bool  active     = false;
	float start_x    = 0.f, start_y = 0.f;
	float last_x     = 0.f, last_y  = 0.f;
	float total_dist = 0.f;
};
TouchState g_touch;
float      g_scroll_pending_px = 0.f;

// ----------------------------------------------------------------------
// Filesystem helpers and resource-root validation
// ----------------------------------------------------------------------
bool PathExists( const std::string &p )
{
	struct stat st;
	return stat( p.c_str(), &st ) == 0;
}

bool IsDir( const std::string &p )
{
	struct stat st;
	return stat( p.c_str(), &st ) == 0 && S_ISDIR( st.st_mode );
}

/* A resource root is valid if it contains "base/assets0.pk3" - the
   engine's hardcoded BASEGAME directory (code/qcommon/q_shared.h,
   BASEGAME "base") and the always-present core asset pak (the other
   three, assets1-3.pk3, are optional map/DLC-less content paks - see
   the comment on files.cpp's pakFilename field:
   "c:\jediacademy\gamedata\base\assets0.pk3"). This is the "GameData"
   folder from a legally installed copy - what fs_basepath points at. */
bool ValidateResourceDir( const std::string &dir )
{
	if( !IsDir( dir )) return false;
	std::string base = dir + "/base";
	if( !IsDir( base )) return false;
	return PathExists( base + "/assets0.pk3" );
}

/* Jedi Academy SP has no official DLC/mod split shipped with the base
   game - nothing to list here. Fan mods would go here as
   { "Mod Name", "moddir" } if the port ever wants to expose fs_game. */
struct ModDef { const char *label; const char *id; };
static const std::vector<ModDef> kMods = {
};

// ----------------------------------------------------------------------
// Persistent config: ~/.config/<org>/<app>/launcher.conf, key=value.
//   path       - last picked resource root
//   r_3d_scale - offscreen render scale factor (FBO)
// ----------------------------------------------------------------------
std::string HomeDir()
{
	const char *home = getenv( "HOME" );
	if( !home || !*home )
	{
		passwd *pw = getpwuid( getuid());
		home = pw ? pw->pw_dir : "/tmp";
	}
	return std::string( home );
}

std::string ConfigDir()
{
	/* The AuroraOS sandbox only allows writes under ~/.config/<org>/<app>
	   (and ~/.local/share, ~/.cache) - elsewhere the write is silently
	   swallowed and the config is lost on the next launch. */
	return HomeDir() + "/.config/" AURORA_ORG "/" AURORA_APP;
}

std::string ConfigFile() { return ConfigDir() + "/launcher.conf"; }

void MakeDirsP( const std::string &path )
{
	std::string acc;
	for( size_t i = 1; i <= path.size(); ++i )
	{
		if( i == path.size() || path[i] == '/' )
		{
			acc.assign( path, 0, i );
			if( !acc.empty()) mkdir( acc.c_str(), 0755 );
		}
	}
}

struct LauncherSettings
{
	std::string path;
	float       r_3d_scale = 1.0f;
};

LauncherSettings g_settings;

std::string TrimStr( const std::string &s )
{
	size_t a = 0, b = s.size();
	while( a < b && ( s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n' )) ++a;
	while( b > a && ( s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n' )) --b;
	return s.substr( a, b - a );
}

void LoadSettings()
{
	FILE *f = fopen( ConfigFile().c_str(), "rb" );
	if( !f ) return;
	char line[4096];
	while( fgets( line, sizeof( line ), f ))
	{
		std::string s = TrimStr( line );
		if( s.empty() || s[0] == '#' ) continue;
		size_t eq = s.find( '=' );
		if( eq == std::string::npos ) continue;
		std::string k = TrimStr( s.substr( 0, eq ));
		std::string v = TrimStr( s.substr( eq + 1 ));
		if( k == "path" )            g_settings.path = v;
		else if( k == "r_3d_scale" ) g_settings.r_3d_scale = (float)atof( v.c_str());
	}
	fclose( f );
	if( g_settings.r_3d_scale < 0.25f ) g_settings.r_3d_scale = 0.25f;
	if( g_settings.r_3d_scale > 2.0f )  g_settings.r_3d_scale = 2.0f;
}

void SaveSettings()
{
	MakeDirsP( ConfigDir());
	FILE *f = fopen( ConfigFile().c_str(), "wb" );
	if( !f ) return;
	fprintf( f, "path=%s\n",         g_settings.path.c_str());
	fprintf( f, "r_3d_scale=%.3f\n", g_settings.r_3d_scale );
	fclose( f );
}

/* Browser start directory: the picked path if any, else the conventional
   AuroraOS location ~/Downloads/Games/<app>, else ~/Downloads. */
std::string DefaultBrowserDir()
{
	if( !g_picker.selected.empty()) return g_picker.selected;
	std::string home = HomeDir();
	std::string games = home + "/Downloads/Games/" AURORA_APP;
	if( IsDir( games ))                    return games;
	if( IsDir( home + "/Downloads" ))      return home + "/Downloads";
	return home;
}

// ----------------------------------------------------------------------
// Touch events: tap vs drag (finger coordinates are normalised 0..1)
// ----------------------------------------------------------------------
void ProcessTouchEvent( const SDL_Event &in, int win_w, int win_h )
{
	if( in.type != SDL_FINGERDOWN && in.type != SDL_FINGERUP && in.type != SDL_FINGERMOTION )
		return;

	/* tfinger.x/y are normalised 0..1 - scale to window pixels. */
	float px = in.tfinger.x * (float)win_w;
	float py = in.tfinger.y * (float)win_h;

	const float TAP_THRESHOLD = 16.f;

	if( in.type == SDL_FINGERDOWN )
	{
		g_touch.active     = true;
		g_touch.start_x    = px;
		g_touch.start_y    = py;
		g_touch.last_x     = px;
		g_touch.last_y     = py;
		g_touch.total_dist = 0.f;

		/* Move the ImGui "mouse" under the finger so the hover test picks
		   the right window for any subsequent scroll. */
		SDL_Event mv = {};
		mv.type = SDL_MOUSEMOTION;
		mv.motion.timestamp = in.tfinger.timestamp;
		mv.motion.which     = SDL_TOUCH_MOUSEID;
		mv.motion.x         = (int)px;
		mv.motion.y         = (int)py;
		SDL_PushEvent( &mv );
	}
	else if( in.type == SDL_FINGERMOTION && g_touch.active )
	{
		float dx = px - g_touch.last_x;
		float dy = py - g_touch.last_y;
		g_touch.total_dist += sqrtf( dx * dx + dy * dy );
		g_touch.last_x = px;
		g_touch.last_y = py;
		/* The finger has clearly moved - the gesture is a scroll. */
		if( g_touch.total_dist >= TAP_THRESHOLD )
			g_scroll_pending_px += dy;
	}
	else if( in.type == SDL_FINGERUP && g_touch.active )
	{
		if( g_touch.total_dist < TAP_THRESHOLD )
		{
			/* Tap - down+up at the original point so ImGui sees a click. */
			SDL_Event d = {};
			d.type = SDL_MOUSEBUTTONDOWN;
			d.button.timestamp = in.tfinger.timestamp;
			d.button.which     = SDL_TOUCH_MOUSEID;
			d.button.button    = SDL_BUTTON_LEFT;
			d.button.state     = SDL_PRESSED;
			d.button.clicks    = 1;
			d.button.x         = (int)g_touch.start_x;
			d.button.y         = (int)g_touch.start_y;
			SDL_PushEvent( &d );

			SDL_Event u = d;
			u.type = SDL_MOUSEBUTTONUP;
			u.button.state = SDL_RELEASED;
			SDL_PushEvent( &u );
		}
		g_touch.active = false;
	}
}

void ApplyPendingScroll()
{
	if( g_scroll_pending_px == 0.f ) return;
	ImGuiIO &io = ImGui::GetIO();
	/* Pixels -> wheel ticks: ImGui scrolls 5*FontSize px per tick
	   (UpdateMouseWheel: scroll_step = min(5*FontSize, 0.67*window
	   height)). Dividing by exactly that step makes the content track the
	   finger 1:1. */
	const float px_per_tick = 5.f * ImGui::GetFontSize();
	io.MouseWheel += g_scroll_pending_px / px_per_tick;
	g_scroll_pending_px = 0.f;
}

// ----------------------------------------------------------------------
// Directory listing helper (subdirectories only)
// ----------------------------------------------------------------------
std::vector<std::string> ListSubdirs( const std::string &dir )
{
	std::vector<std::string> out;
	DIR *d = opendir( dir.c_str());
	if( !d ) return out;
	dirent *ent;
	while(( ent = readdir( d )))
	{
		const char *n = ent->d_name;
		if( n[0] == '.' ) continue; /* skip hidden entries and . / .. */
		std::string full = dir + "/" + n;
		if( IsDir( full )) out.push_back( n );
	}
	closedir( d );
	std::sort( out.begin(), out.end(), []( const std::string &a, const std::string &b )
	{
		return strcasecmp( a.c_str(), b.c_str()) < 0;
	});
	return out;
}

std::string ParentOf( const std::string &dir )
{
	if( dir.empty() || dir == "/" ) return "/";
	size_t s = dir.find_last_of( '/' );
	if( s == std::string::npos || s == 0 ) return "/";
	return dir.substr( 0, s );
}

// ----------------------------------------------------------------------
// GL attributes: mirror the set the engine requests for its own window/
// context (shared/sdl/sdl_window.cpp GLimp_SetMode - GLPROFILE_ES 3.0,
// RGBA 8/8/8/8, DEPTH 24, STENCIL 8, doublebuffer). Keep these in sync.
// ----------------------------------------------------------------------
void SetGLAttributesForEngine()
{
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 0 );
#ifdef SDL_HINT_VIDEO_X11_FORCE_EGL
	SDL_SetHint( SDL_HINT_VIDEO_X11_FORCE_EGL, "1" );
#endif
#ifdef SDL_HINT_OPENGL_ES_DRIVER
	SDL_SetHint( SDL_HINT_OPENGL_ES_DRIVER, "1" );
#endif

	SDL_GL_SetAttribute( SDL_GL_RED_SIZE,     8 );
	SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE,   8 );
	SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE,    8 );
	SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE,   8 );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE,  24 );
	SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
}

// ----------------------------------------------------------------------
// Directory browser (on top of the main launcher window)
// ----------------------------------------------------------------------
void DrawDirectoryBrowser( int win_w, int win_h )
{
	if( !g_picker.browser_open ) return;

	const float fs = ES();
	ImGui::SetNextWindowPos(  ImVec2( fs * 0.5f, fs * 0.5f ));
	ImGui::SetNextWindowSize( ImVec2( win_w - fs, win_h - fs ));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoSavedSettings;
	ImGui::Begin( "##browser", nullptr, flags );

	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( fs * 0.6f, fs * 0.5f ));
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,  ImVec2( fs * 0.4f, fs * 0.6f ));

	ImGui::Text( "Текущий путь:" );
	ImGui::TextWrapped( "%s", g_picker.current_dir.c_str());
	ImGui::Spacing();

	const float row_h = fs * 2.5f * kBtnScale;

	/* Action buttons span the full width and stack vertically (touch). */
	if( ImGui::Button( "Выбрать эту папку", ImVec2( -1, row_h )))
	{
		g_picker.selected     = g_picker.current_dir;
		g_picker.valid_pick   = ValidateResourceDir( g_picker.selected );
		g_picker.browser_open = false;
	}
	if( ImGui::Button( "Вверх", ImVec2( -1, row_h )))
		g_picker.current_dir = ParentOf( g_picker.current_dir );
	if( ImGui::Button( "Отмена", ImVec2( -1, row_h )))
		g_picker.browser_open = false;

	ImGui::Separator();

	/* Subdirectories - a flat list (visually NOT buttons): Selectable with
	   no background, left-aligned text, a "[  ]" marker before the name
	   (no emoji in the font - the glyph ranges don't include them). The
	   first entry is always "[  ] .." - go up one level. Every entry is
	   inset 3 mm from both edges of the scroll view (the launcher font is
	   4.5 mm, so compute from the real font size, not the squashed ES). */
	const float pad_x  = ImGui::GetFontSize() * ( 3.f / 4.5f );
	const float item_h = row_h * 0.9f;
	ImGui::BeginChild( "##dir_list", ImVec2( 0, 0 ), false,
		ImGuiWindowFlags_AlwaysVerticalScrollbar );

	/* Highlight entries only while the finger is on the screen. After
	   FINGERUP the synthetic mouse stays at the tap point, and the entry
	   with the same index in the NEW list would stay "hovered" - without a
	   touch, suppress hover/active. */
	const bool highlight = g_touch.active;
	if( !highlight )
	{
		ImGui::PushStyleColor( ImGuiCol_HeaderHovered, ImVec4( 0, 0, 0, 0 ));
		ImGui::PushStyleColor( ImGuiCol_HeaderActive,  ImVec4( 0, 0, 0, 0 ));
	}

	auto drawItem = [&]( const char *label ) -> bool
	{
		ImGui::SetCursorPosX( ImGui::GetCursorPosX() + pad_x );
		return ImGui::Selectable( label, false, 0,
			ImVec2( ImGui::GetContentRegionAvail().x - pad_x, item_h ));
	};

	if( drawItem( "[  ] .." ))
		g_picker.current_dir = ParentOf( g_picker.current_dir );

	auto subs = ListSubdirs( g_picker.current_dir );
	for( const auto &name : subs )
	{
		std::string label = "[  ] " + name;
		if( drawItem( label.c_str()))
		{
			std::string next = g_picker.current_dir;
			if( next != "/" ) next += "/";
			next += name;
			g_picker.current_dir = next;
		}
	}

	if( !highlight )
		ImGui::PopStyleColor( 2 );

	ImGui::EndChild();

	ImGui::PopStyleVar( 2 );
	ImGui::End();
}

// ----------------------------------------------------------------------
// Tab "Игра" (Game)
// ----------------------------------------------------------------------
void StartGame()
{
	g_launch = true;
}

void DrawTab_Game()
{
	const float fs    = ES();
	const float btn_h = fs * 3.3f * kBtnScale;

	ImGui::Text( "Путь к ресурсам:" );
	ImGui::TextWrapped( "%s", g_picker.selected.empty()
		? "(не выбран)"
		: g_picker.selected.c_str());

	ImGui::Spacing();
	if( ImGui::Button( "Выбрать папку...", ImVec2( -1, btn_h )))
		g_browser_open_pending = true; /* opens on the next frame */

	ImGui::Spacing();
	if( g_picker.selected.empty())
	{
		ImGui::TextColored( ImVec4( 0.9f, 0.7f, 0.2f, 1.f ),
			"Выберите папку с ресурсами игры (GameData)" );
	}
	else if( g_picker.valid_pick )
	{
		ImGui::TextColored( ImVec4( 0.4f, 0.9f, 0.4f, 1.f ),
			"Ресурсы найдены." );
	}
	else
	{
		ImGui::TextColored( ImVec4( 0.95f, 0.4f, 0.4f, 1.f ),
			"В выбранной папке не найдены ресурсы игры (нет base/assets0.pk3)." );
	}

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));

	const bool can_launch = !g_picker.selected.empty() && g_picker.valid_pick;
	ImGui::BeginDisabled( !can_launch );

	if( ImGui::Button( "Начать игру", ImVec2( -1, btn_h )))
		StartGame();

	ImGui::EndDisabled();
}

// ----------------------------------------------------------------------
// Tab "Настройки" (Settings)
// ----------------------------------------------------------------------
void DrawTab_Settings()
{
	const float fs = ES();

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));
	ImGui::TextWrapped( "Разрешение рендера (3D scale)" );
	ImGui::TextWrapped(
		"Множитель размера буфера рендеринга относительно экрана. "
		"0.5 = половина разрешения (быстрее), 1.0 = полное, "
		"2.0 = supersampling (чётче, но медленнее)." );
	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));

	ImGui::PushItemWidth( -fs * 4.f );
	ImGui::SliderFloat( "##r_3d_scale", &g_settings.r_3d_scale, 0.25f, 2.0f, "%.2f" );
	ImGui::PopItemWidth();

	ImGui::Spacing();
	/* Presets - large finger targets. */
	const float btn_h = fs * 3.f * kBtnScale;
	const float btn_w = (( ImGui::GetContentRegionAvail().x - fs * 2.f ) / 5.f );
	auto preset = [&]( const char *label, float value )
	{
		if( ImGui::Button( label, ImVec2( btn_w, btn_h )))
			g_settings.r_3d_scale = value;
	};
	preset( "0.25", 0.25f ); ImGui::SameLine();
	preset( "0.50", 0.50f ); ImGui::SameLine();
	preset( "0.75", 0.75f ); ImGui::SameLine();
	preset( "1.0",  1.00f ); ImGui::SameLine();
	preset( "2.0",  2.00f );

	ImGui::Dummy( ImVec2( 0, fs ));
	ImGui::TextColored( ImVec4( 0.6f, 0.8f, 1.f, 1.f ),
		"Текущее значение: %.2f", g_settings.r_3d_scale );
}

// ----------------------------------------------------------------------
// Tab "О программе" (About)
// ----------------------------------------------------------------------
void DrawTab_About()
{
	ImGui::TextWrapped(
		"Программа предоставляется AS IS, без каких-либо гарантий.\n\n"
		"Ресурсы игры не распространяются с этим приложением - "
		"приобретите игру легально и укажите путь к ним во вкладке «Игра»." );
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Движок: OpenJK (форк оригинального движка Star Wars Jedi Knight: "
		"Jedi Academy), лицензия GPL-2.0+.\n"
		"Исходный код порта:" );

	/* Clickable link: underlined text in the theme accent colour, opened
	   via SDL_OpenURL on tap. */
	auto DrawLink = [&]( const char *text, const char *url )
	{
		const ImVec4 accent( 0x3B / 255.f, 0x82 / 255.f, 0xF6 / 255.f, 1.f );
		ImGui::PushStyleColor( ImGuiCol_Text, accent );
		ImGui::TextUnformatted( text );
		ImGui::PopStyleColor();
		ImVec2 rmin = ImGui::GetItemRectMin();
		ImVec2 rmax = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2( rmin.x, rmax.y ), ImVec2( rmax.x, rmax.y ),
			ImGui::GetColorU32( accent ));
		if( ImGui::IsItemClicked())
			SDL_OpenURL( url );
	};

	DrawLink( "github.com/savegame/aurora-openjk.git",
		"https://github.com/savegame/aurora-openjk.git" );

	ImGui::Spacing();
	ImGui::TextWrapped(
		"Подписывайтесь в телеграм. Свежие порты игр и новости об ОС Аврора:" );
	DrawLink( "sashikknox Все портит! - t.me/auroraosgames",
		"https://t.me/auroraosgames" );

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Сторонние компоненты:\n"
		"  Dear ImGui (c) Omar Cornut, лицензия MIT\n"
		"  SDL2, лицензия zlib\n"
		"  OpenJK contributors, лицензия GPL-2.0+" );
}

// ----------------------------------------------------------------------
// Tab row: custom buttons in the theme style (the stock ImGui TabBar is
// blue and doesn't fit), rounded at the top only. The row itself is fixed
// - only the content below it scrolls.
// ----------------------------------------------------------------------
int g_active_tab = 0;

void DrawTabsRow()
{
	static const char *names[] = { "Игра", "Настройки", "О программе" };
	const float fs       = ES();
	const float tab_h    = fs * 2.6f * kTabScale;
	const float gap      = fs * 0.4f;
	const float avail    = ImGui::GetContentRegionAvail().x;
	const float tab_w    = ( avail - gap * 2.f ) / 3.f;
	const float rounding = fs * 0.8f;

	/* Colours come from the active theme: the active tab is accented, the
	   rest look like inactive frames. */
	const ImVec4 accent  = ImGui::GetStyleColorVec4( ImGuiCol_SliderGrab );
	const ImVec4 normal  = ImGui::GetStyleColorVec4( ImGuiCol_FrameBg );
	const ImVec4 hovered = ImGui::GetStyleColorVec4( ImGuiCol_FrameBgHovered );

	ImDrawList *dl = ImGui::GetWindowDrawList();
	for( int i = 0; i < 3; i++ )
	{
		if( i > 0 )
			ImGui::SameLine( 0.f, gap );

		ImGui::PushID( i );
		ImGui::InvisibleButton( "##tab", ImVec2( tab_w, tab_h ));
		const bool sel = ( g_active_tab == i );
		if( ImGui::IsItemClicked())
			g_active_tab = i;

		const ImVec4 col  = sel ? accent : ( ImGui::IsItemHovered() ? hovered : normal );
		const ImVec2 rmin = ImGui::GetItemRectMin();
		const ImVec2 rmax = ImGui::GetItemRectMax();
		dl->AddRectFilled( rmin, rmax, ImGui::GetColorU32( col ),
			rounding, ImDrawFlags_RoundCornersTop );

		const ImVec2 ts = ImGui::CalcTextSize( names[i] );
		dl->AddText( ImVec2( rmin.x + ( tab_w - ts.x ) * 0.5f,
				rmin.y + ( tab_h - ts.y ) * 0.5f ),
			ImGui::GetColorU32( ImGuiCol_Text ), names[i] );
		ImGui::PopID();
	}
}

// ----------------------------------------------------------------------
// Root launcher window (fullscreen, undecorated)
// ----------------------------------------------------------------------
void DrawLauncherUI( bool &user_quit, int win_w, int win_h )
{
	/* Deferred browser open - here, at the start of the frame: the tap
	   release that opened it is already in the previous frame and can't
	   fall through into the window. */
	if( g_browser_open_pending )
	{
		g_browser_open_pending = false;
		g_picker.current_dir   = DefaultBrowserDir();
		g_picker.browser_open  = true;
	}

	ImGui::SetNextWindowPos(  ImVec2( 0, 0 ));
	ImGui::SetNextWindowSize( ImVec2( (float)win_w, (float)win_h ));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoSavedSettings;

	ImGui::Begin( "##launcher", nullptr, flags );

	const float fs = ES();
	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( fs * 1.0f, fs * 0.6f ));
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,  ImVec2( fs * 0.5f, fs * 0.6f ));

	ImVec2 hdr = ImGui::CalcTextSize( GAME_TITLE " (AuroraOS)" );
	ImGui::SetCursorPos( ImVec2(( win_w - hdr.x ) * 0.5f, fs * 1.2f ));
	ImGui::TextUnformatted( GAME_TITLE " (AuroraOS)" );

	/* Tab row - fixed, not scrollable. */
	DrawTabsRow();
	ImGui::Spacing();

	/* Active tab content lives in a scrollable child: with the large font
	   it can overflow the screen height. */
	const float exit_h = fs * 3.3f * kBtnScale;
	const float avail_h = win_h - ImGui::GetCursorPosY() - exit_h
		- fs * 1.5f - ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild( "##content", ImVec2( 0, avail_h ), false );

	switch( g_active_tab )
	{
	case 0:  DrawTab_Game();     break;
	case 1:  DrawTab_Settings(); break;
	default: DrawTab_About();    break;
	}

	ImGui::EndChild();

	/* "Exit" - at the bottom, full width. */
	ImGui::SetCursorPosY( win_h - exit_h - fs * 1.5f );
	if( ImGui::Button( "Выход", ImVec2( -1, exit_h )))
		user_quit = true;

	ImGui::PopStyleVar( 2 );
	ImGui::End();

	DrawDirectoryBrowser( win_w, win_h );
}

/* One "LOADING" frame before handing control to the engine: engine init
   and the first asset load take a noticeable while, and a frozen frame
   with a pressed button looks broken. */
void DrawLoadingFrame()
{
	int win_w = 0, win_h = 0;
	SDL_GetWindowSize( g_window, &win_w, &win_h );

	/* Drain queued events so NewFrame sees a clean state. */
	SDL_Event drain;
	while( SDL_PollEvent( &drain ))
		ImGui_ImplSDL2_ProcessEvent( &drain );

	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();

	ImGui::SetNextWindowPos(  ImVec2( 0, 0 ));
	ImGui::SetNextWindowSize( ImVec2( (float)win_w, (float)win_h ));
	ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoScrollbar;
	ImGui::Begin( "##loading", nullptr, wflags );
	const char *txt = "ЗАГРУЗКА";
	ImVec2 sz = ImGui::CalcTextSize( txt );
	ImGui::SetCursorPos( ImVec2(( win_w - sz.x ) * 0.5f, ( win_h - sz.y ) * 0.5f ));
	ImGui::TextUnformatted( txt );
	ImGui::End();

	ImGui::Render();
	glViewport( 0, 0, win_w, win_h );
	glClearColor( 0.08f, 0.08f, 0.10f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
	SDL_GL_SwapWindow( g_window );
}

} // namespace

extern "C" {

launcher_result_t Launcher_Run( void )
{
	/* Don't let SDL synthesise mouse events from touch: the launcher
	   converts taps/drags itself (ProcessTouchEvent), and with synthesis
	   enabled every tap arrives twice (SDL's pair + ours) - double clicks
	   on buttons. Set BEFORE SDL_Init(SDL_INIT_VIDEO): the hint value is
	   picked up by a callback during mouse initialisation. */
	SDL_SetHint( SDL_HINT_TOUCH_MOUSE_EVENTS, "0" );

	/* SDL_INIT_VIDEO was normally already done by the engine's platform
	   init before we got here. If it wasn't, bring it up. */
	if( SDL_WasInit( SDL_INIT_VIDEO ) == 0 )
	{
		if( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_EVENTS ) < 0 )
		{
			fprintf( stderr, "Launcher: SDL_Init failed: %s\n", SDL_GetError() );
			return LAUNCHER_QUIT;
		}
	}

	SetGLAttributesForEngine();

	/* Window size: prefer usable bounds (on a portrait panel that's e.g.
	   1080x2400 - already accounting for the compositor transform), fall
	   back to the desktop mode. Do NOT use a bare SDL_GetDesktopDisplayMode:
	   it can report the panel's native landscape (e.g. 2400x1080) without
	   rotation - the window comes out landscape and the FBO blit targets a
	   wrong viewport. */
	int win_w = 1080, win_h = 1920;
	{
		SDL_Rect bounds;
		if( SDL_GetDisplayUsableBounds( 0, &bounds ) == 0 && bounds.w > 0 && bounds.h > 0 )
		{
			win_w = bounds.w;
			win_h = bounds.h;
		}
		else
		{
			SDL_DisplayMode dm;
			if( SDL_GetDesktopDisplayMode( 0, &dm ) == 0 )
			{
				win_w = dm.w;
				win_h = dm.h;
			}
		}
		printf( "Launcher: window %dx%d\n", win_w, win_h );
		fflush( stdout );
	}

	g_window = SDL_CreateWindow( GAME_TITLE " (AuroraOS)",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		win_w, win_h,
		SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN );
	if( !g_window )
	{
		fprintf( stderr, "Launcher: SDL_CreateWindow failed: %s\n", SDL_GetError() );
		return LAUNCHER_QUIT;
	}

	g_context = SDL_GL_CreateContext( g_window );
	if( !g_context )
	{
		fprintf( stderr, "Launcher: SDL_GL_CreateContext failed: %s\n", SDL_GetError() );
		SDL_DestroyWindow( g_window );
		g_window = nullptr;
		return LAUNCHER_QUIT;
	}

	SDL_GL_MakeCurrent( g_window, g_context );
	SDL_GL_SetSwapInterval( 1 );

	g_owned = true;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr; /* don't write imgui.ini (sandbox) */
	io.LogFilename = nullptr;

	/* Font size from the physical DPI: target ~4.5 mm (clamp 14..96 px so
	   a broken/zero DPI can't produce a microscopic or gigantic font). */
	float vdpi = 96.f;
	SDL_GetDisplayDPI( SDL_GetWindowDisplayIndex( g_window ), nullptr, nullptr, &vdpi );
	if( vdpi <= 1.f ) vdpi = 96.f;
	float font_px = 4.5f * ( vdpi / 25.4f );
	if( font_px < 14.f ) font_px = 14.f;
	if( font_px > 96.f ) font_px = 96.f;

	/* Theme: palette + metrics from docs/imgui-theme-spec.md. */
	AuroraLauncher_ApplyTheme( font_px );
	/* Launcher widgets 20% more compact at the same font (paddings,
	   roundings and other theme metrics; explicit sizes go through ES()). */
	ImGui::GetStyle().ScaleAllSizes( kElemScale );

	/* Font - embedded Noto Sans Bold (glyph ranges listed explicitly:
	   Latin-1 + Cyrillic + General Punctuation; the em-dash 0x2014 etc.
	   are NOT in GetGlyphRangesCyrillic and would render as "?"). Emoji
	   ranges deliberately excluded. */
	static const ImWchar kGlyphRanges[] = {
		0x0020, 0x00FF, /* Latin-1 + punctuation */
		0x0400, 0x04FF, /* Cyrillic */
		0x2010, 0x205E, /* General Punctuation (- „ " " …) */
		0,
	};
	if( io.Fonts->AddFontFromMemoryCompressedTTF(
		NotoSansBold_compressed_data, NotoSansBold_compressed_size,
		font_px, nullptr, kGlyphRanges ) == nullptr )
	{
		/* Fall back to ImGui's built-in font, scaled. */
		io.FontGlobalScale = font_px / 13.f;
	}

	ImGui_ImplSDL2_InitForOpenGL( g_window, g_context );
	ImGui_ImplOpenGL3_Init( "#version 300 es" );

	/* Config + resource pre-pick: the saved path, else the conventional
	   ~/Downloads/Games/<app> - if valid, the user just presses Start. */
	LoadSettings();
	{
		std::string defdir = HomeDir() + "/Downloads/Games/" AURORA_APP;
		if( !g_settings.path.empty() && ValidateResourceDir( g_settings.path ))
		{
			g_picker.selected   = g_settings.path;
			g_picker.valid_pick = true;
		}
		else if( ValidateResourceDir( defdir ))
		{
			g_picker.selected   = defdir;
			g_picker.valid_pick = true;
		}
	}

	bool user_quit = false;

	/* Debug autostart (env AURORA_LAUNCHER_AUTO=1): after ~2.5 s call the
	   same callback the start button would. Used for host verification -
	   see rpm-less builds via host_build.sh. */
	const bool auto_start = getenv( "AURORA_LAUNCHER_AUTO" ) != nullptr;
	const Uint32 loop_start = SDL_GetTicks();
	bool auto_fired = false;

	while( !g_launch && !user_quit )
	{
		SDL_GetWindowSize( g_window, &win_w, &win_h );

		SDL_Event ev;
		while( SDL_PollEvent( &ev ))
		{
			ImGui_ImplSDL2_ProcessEvent( &ev );

			if( ev.type == SDL_QUIT )
				user_quit = true;
			else if( ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERUP || ev.type == SDL_FINGERMOTION )
				ProcessTouchEvent( ev, win_w, win_h );
		}

		if( auto_start && !auto_fired && SDL_GetTicks() - loop_start >= 2500 )
		{
			auto_fired = true;
			StartGame();
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ApplyPendingScroll();
		ImGui::NewFrame();

		DrawLauncherUI( user_quit, win_w, win_h );

		ImGui::Render();
		glViewport( 0, 0, win_w, win_h );
		glClearColor( 0.08f, 0.08f, 0.10f, 1.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );

		SDL_GL_SwapWindow( g_window );
	}

	if( g_launch )
		g_settings.path = g_picker.selected;

	/* Save the config on quit too - settings must not be lost. */
	SaveSettings();

	if( g_launch )
		DrawLoadingFrame();

	/* Tear down ImGui - it releases its own VAO/VBO/program/font texture.
	   The SDL window and GL context stay alive and become the engine's.
	   No SDL_Quit here. */
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();

	if( !g_launch )
	{
		/* User asked to quit before the engine ran. Tear down our own
		   window/context. */
		if( g_context ) { SDL_GL_DeleteContext( g_context ); g_context = nullptr; }
		if( g_window  ) { SDL_DestroyWindow ( g_window  );  g_window  = nullptr; }
		g_owned = false;
		return LAUNCHER_QUIT;
	}

	/* Hand the bare GL context back to a clean state for the engine. */
	SDL_GL_MakeCurrent( g_window, g_context );
	return LAUNCHER_CONTINUE;
}

void *Launcher_GetWindow( void )
{
	return g_owned ? (void *)g_window : nullptr;
}

void *Launcher_GetGLContext( void )
{
	return g_owned ? (void *)g_context : nullptr;
}

void Launcher_ReleaseOwnership( void )
{
	/* Engine has adopted the window/context; we no longer free them. */
	g_owned = false;
}

const char *Launcher_GetResourceDir( void )
{
	return g_picker.selected.c_str();
}

float Launcher_GetFboScale( void )
{
	return g_settings.r_3d_scale;
}

} // extern "C"
