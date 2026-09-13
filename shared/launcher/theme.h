/*
theme.h - AuroraOS launcher ImGui theme
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

#pragma once
#ifndef LAUNCHER_THEME_H
#define LAUNCHER_THEME_H

// Apply the port's ImGui theme: the palette and the base metrics grid from
// docs/imgui-theme-spec.md, with the whole grid scaled by the actual font
// size (fontSizePx / 13). Call after ImGui::CreateContext.
void AuroraLauncher_ApplyTheme( float fontSizePx );

#endif // LAUNCHER_THEME_H
