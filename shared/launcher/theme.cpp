/*
theme.cpp - AuroraOS launcher ImGui theme
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

// Palette + metrics grid from docs/imgui-theme-spec.md. The grid is defined
// for a 13 px font and scaled by (fontSizePx / 13) so every metric tracks
// the physical font size. The one deliberate deviation from the spec is the
// frame rounding: the spec says 2 px, the port's signature look rounds
// buttons by a full font size (set AFTER ScaleAllSizes so it isn't scaled).

#include "theme.h"

#include "imgui.h"

#define THEME_HEX(h) ImVec4(((h >> 16) & 0xFF) / 255.f, ((h >> 8) & 0xFF) / 255.f, (h & 0xFF) / 255.f, 1.0f)

void AuroraLauncher_ApplyTheme( float fontSizePx )
{
	ImGui::StyleColorsDark();

	ImGuiStyle &s = ImGui::GetStyle();
	s.WindowPadding      = ImVec2( 10, 10 );
	s.FramePadding       = ImVec2( 6, 3 );
	s.ItemSpacing        = ImVec2( 6, 4 );
	s.ItemInnerSpacing   = ImVec2( 4, 4 );
	s.IndentSpacing      = 12.0f;
	s.WindowBorderSize   = 1.0f;
	s.FrameBorderSize    = 1.0f;
	s.WindowRounding     = 0.0f;
	s.FrameRounding      = 2.0f;
	s.GrabRounding       = 1.0f;
	s.ScrollbarSize      = 10.0f;
	s.TabRounding        = 0.0f;
	s.ScaleAllSizes( fontSizePx / 13.0f );

	// The port's signature button rounding (~30% of a ~3.3-font button
	// height). Set after ScaleAllSizes on purpose.
	s.FrameRounding      = fontSizePx;

	ImVec4 *c = s.Colors;
	c[ImGuiCol_WindowBg]         = THEME_HEX( 0x1F1F22 );
	c[ImGuiCol_ChildBg]          = THEME_HEX( 0x16161A );
	c[ImGuiCol_PopupBg]          = THEME_HEX( 0x232328 );
	c[ImGuiCol_FrameBg]          = THEME_HEX( 0x2A2A2F );
	c[ImGuiCol_FrameBgHovered]   = THEME_HEX( 0x34343A );
	c[ImGuiCol_FrameBgActive]    = THEME_HEX( 0x3D3D44 );
	c[ImGuiCol_Button]           = THEME_HEX( 0x34343A );
	c[ImGuiCol_ButtonHovered]    = THEME_HEX( 0x3F3F46 );
	c[ImGuiCol_ButtonActive]     = THEME_HEX( 0x4A4A52 );
	c[ImGuiCol_Header]           = THEME_HEX( 0x2E2E34 );
	c[ImGuiCol_HeaderHovered]    = THEME_HEX( 0x383840 );
	c[ImGuiCol_HeaderActive]     = THEME_HEX( 0x424249 );
	c[ImGuiCol_Border]           = THEME_HEX( 0x0B0B0D );
	c[ImGuiCol_Separator]        = THEME_HEX( 0x2E2E33 );
	c[ImGuiCol_Text]             = THEME_HEX( 0xE6E6EA );
	c[ImGuiCol_TextDisabled]     = THEME_HEX( 0x5D5D66 );
	c[ImGuiCol_CheckMark]        = THEME_HEX( 0xFFFFFF );
	c[ImGuiCol_SliderGrab]       = THEME_HEX( 0x3B82F6 ); /* accent */
	c[ImGuiCol_SliderGrabActive] = THEME_HEX( 0x5AA0FF ); /* accent line */
}
