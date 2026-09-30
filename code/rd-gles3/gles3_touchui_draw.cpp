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

// Aurora touch-UI - ImGui-facing half (see gles3_touchui_draw.h for why this
// file must never include tr_local.h/vk_local.h).
//
// A private ImGui context, fed no input at all: only
// ImGui::GetForegroundDrawList() is used to draw a handful of procedural
// primitives (lines/circles/polylines), the same "no image assets" trick
// used by the reference port quake4es
// (~/Projects/aurora-ports/quake4/quake4es/doom3/neo/renderer/imgui/
// r_touch.cpp, RB_TouchIcon) - read there, adapted to this port's icon set
// (JKA has no flashlight/PDA, but does have force powers and an alt-fire).
// Hit-testing lives entirely in the client (shared/sdl/sdl_touchui.cpp);
// this context never receives a mouse/touch event, so ImGui's own input
// state (hover, active id, drag) never enters the picture here at all.

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "gles3_touchui_draw.h"

#include <math.h>
#include <string.h>
#include <cfloat>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static ImGuiContext *aurora_touchImGuiCtx = NULL;
static bool aurora_touchImGuiFailed = false;

/*
====================
Aurora_TouchUI_EnsureInit

Made on the first frame there is something to draw, with the renderer's GL
context already current. Kept until Aurora_TouchUI_ShutdownDraw().
====================
*/
static bool Aurora_TouchUI_EnsureInit( void )
{
	if ( aurora_touchImGuiCtx )
	{
		return true;
	}
	if ( aurora_touchImGuiFailed )
	{
		return false;
	}

	ImGuiContext *previous = ImGui::GetCurrentContext();

	aurora_touchImGuiCtx = ImGui::CreateContext();
	ImGui::SetCurrentContext( aurora_touchImGuiCtx );

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = NULL;
	io.LogFilename = NULL;
	// No text is drawn (every button is an icon), but ImGui still wants a
	// non-empty font atlas before the first NewFrame()/Render() - the
	// built-in pixel font costs one small texture and is never actually
	// sampled for glyphs by this module.
	io.Fonts->AddFontDefault();

	bool ok = ImGui_ImplOpenGL3_Init( "#version 300 es" );

	if ( !ok )
	{
		ImGui::DestroyContext( aurora_touchImGuiCtx );
		aurora_touchImGuiCtx = NULL;
		aurora_touchImGuiFailed = true;
	}

	ImGui::SetCurrentContext( previous );
	return ok;
}

static ImU32 Aurora_TouchColor( int r, int g, int b, float alpha )
{
	if ( alpha < 0.0f ) alpha = 0.0f;
	if ( alpha > 1.0f ) alpha = 1.0f;
	return IM_COL32( r, g, b, (int)( alpha * 255.0f + 0.5f ) );
}

/*
====================
Aurora_TouchUI_RotateOffset

Icon shapes are defined as offsets from their own centre in VISUAL/
landscape local axes (e.g. an arrow's "up" is (0,-1)) - see
gles3_touchui_draw.h's comment. `transform` is the same WL_OUTPUT_TRANSFORM_*
value the client's Aurora_TouchUI_VisualToWindow (sdl_touchui.cpp) already
uses to turn a button's CENTRE point from visual into window space; this is
the exact same table with the translation stripped out (a pure direction
needs no re-centring, only the rotation) - see
research/touch_ui_research.md п.8 for the shared derivation, and
sdl_touchui.cpp's own copy of this table for the position side.
====================
*/
static void Aurora_TouchUI_RotateOffset( float dx, float dy, int transform, float *rx, float *ry )
{
	switch ( transform )
	{
		default:
		case 0: *rx =  dx; *ry =  dy; break;
		case 1: *rx =  dy; *ry = -dx; break;
		case 2: *rx = -dx; *ry = -dy; break;
		case 3: *rx = -dy; *ry =  dx; break;
	}
}

/*
====================
Aurora_TouchArrowHead

A chevron with its tip at "tip", pointing along dir (unit length, already
rotated into window space by the caller) - same construction as the
reference port's RB_TouchArrowHead.
====================
*/
static void Aurora_TouchArrowHead( ImDrawList *list, const ImVec2 &tip, const ImVec2 &dir, float size, ImU32 color, float thickness )
{
	const ImVec2 back( tip.x - dir.x * size, tip.y - dir.y * size );
	const ImVec2 side( -dir.y * size, dir.x * size );
	ImVec2 points[3];

	points[0] = ImVec2( back.x + side.x, back.y + side.y );
	points[1] = tip;
	points[2] = ImVec2( back.x - side.x, back.y - side.y );

	list->AddPolyline( points, 3, color, ImDrawFlags_None, thickness );
}

/*
====================
Aurora_TouchIcon

Every icon is a handful of lines/circles around centre c, s is roughly the
half-size of the icon. No image assets anywhere. Every local offset is
routed through Aurora_TouchUI_RotateOffset (via the P() helper below) before
being added to c, so an icon drawn as "pointing up" in visual/landscape
terms still points up on screen at any of the 4 physical orientations -
symmetric shapes (Fire's crosshair, Altfire's X, Use's rings) are
unaffected by this (a circle/4-fold symbol looks the same rotated by a
multiple of 90 degrees), asymmetric ones (Menu's bars, Jump's and Crouch's
arrows) are the ones this actually fixes.
====================
*/
static void Aurora_TouchIcon( ImDrawList *list, auroraTouchIcon_t icon, const ImVec2 &c, float s, ImU32 color, float thickness, int transform )
{
	// Local helper: c + RotateOffset(dx,dy) - every icon case below builds
	// its points through this instead of raw "c.x + dx, c.y + dy".
	struct Local {
		static ImVec2 P( const ImVec2 &c, float dx, float dy, int t )
		{
			float rx, ry;
			Aurora_TouchUI_RotateOffset( dx, dy, t, &rx, &ry );
			return ImVec2( c.x + rx, c.y + ry );
		}
		static ImVec2 V( float dx, float dy, int t )
		{
			float rx, ry;
			Aurora_TouchUI_RotateOffset( dx, dy, t, &rx, &ry );
			return ImVec2( rx, ry );
		}
	};

	switch ( icon )
	{
		case AURORA_TOUCH_ICON_MENU:
		{
			for ( int i = -1; i <= 1; i++ )
			{
				const float dy = i * s * 0.55f;
				list->AddLine( Local::P( c, -s * 0.75f, dy, transform ), Local::P( c, s * 0.75f, dy, transform ), color, thickness );
			}
			break;
		}

		case AURORA_TOUCH_ICON_FIRE:
		{
			// a crosshair
			list->AddCircle( c, s * 0.55f, color, 0, thickness );
			for ( int i = 0; i < 4; i++ )
			{
				const float dx = ( i == 0 ) ? 1.0f : ( i == 1 ) ? -1.0f : 0.0f;
				const float dy = ( i == 2 ) ? 1.0f : ( i == 3 ) ? -1.0f : 0.0f;
				list->AddLine( Local::P( c, dx * s * 0.3f, dy * s * 0.3f, transform ),
					Local::P( c, dx * s * 1.05f, dy * s * 1.05f, transform ), color, thickness );
			}
			break;
		}

		case AURORA_TOUCH_ICON_ALTFIRE:
		{
			// a crosshair's circle with a cross inside, to read as "the other button"
			list->AddCircle( c, s * 0.6f, color, 0, thickness );
			list->AddLine( Local::P( c, -s * 0.45f, -s * 0.45f, transform ), Local::P( c, s * 0.45f, s * 0.45f, transform ), color, thickness );
			list->AddLine( Local::P( c, -s * 0.45f, s * 0.45f, transform ), Local::P( c, s * 0.45f, -s * 0.45f, transform ), color, thickness );
			break;
		}

		case AURORA_TOUCH_ICON_JUMP:
		{
			const ImVec2 tip = Local::P( c, 0.0f, -s * 0.9f, transform );
			list->AddLine( Local::P( c, 0.0f, s * 0.9f, transform ), tip, color, thickness );
			Aurora_TouchArrowHead( list, tip, Local::V( 0.0f, -1.0f, transform ), s * 0.6f, color, thickness );
			break;
		}

		case AURORA_TOUCH_ICON_CROUCH:
		{
			// an arrow down onto the floor
			const ImVec2 tip = Local::P( c, 0.0f, s * 0.45f, transform );
			list->AddLine( Local::P( c, 0.0f, -s * 0.95f, transform ), tip, color, thickness );
			Aurora_TouchArrowHead( list, tip, Local::V( 0.0f, 1.0f, transform ), s * 0.55f, color, thickness );
			list->AddLine( Local::P( c, -s * 0.9f, s * 0.95f, transform ), Local::P( c, s * 0.9f, s * 0.95f, transform ), color, thickness );
			break;
		}

		case AURORA_TOUCH_ICON_USE:
		{
			// concentric rings - "press here"
			list->AddCircle( c, s * 0.35f, color, 0, thickness );
			list->AddCircle( c, s * 0.75f, color, 0, thickness * 0.75f );
			break;
		}

		case AURORA_TOUCH_ICON_FORCE:
		{
			// a filled centre with rays, like a spark
			list->AddCircleFilled( c, s * 0.28f, color );
			for ( int i = 0; i < 8; i++ )
			{
				const float a = (float)M_PI * 0.25f * (float)i;
				const float x = cosf( a );
				const float y = sinf( a );
				list->AddLine( Local::P( c, x * s * 0.5f, y * s * 0.5f, transform ),
					Local::P( c, x * s * 0.95f, y * s * 0.95f, transform ), color, thickness );
			}
			break;
		}

		case AURORA_TOUCH_ICON_FORCE_PREV:
		{
			// a chevron/arrowhead alone (no stem) pointing left - reads as
			// a "previous" control, distinct from JUMP's full arrow+line.
			const ImVec2 tip = Local::P( c, -s * 0.5f, 0.0f, transform );
			Aurora_TouchArrowHead( list, tip, Local::V( -1.0f, 0.0f, transform ), s * 0.9f, color, thickness );
			break;
		}

		case AURORA_TOUCH_ICON_FORCE_NEXT:
		{
			const ImVec2 tip = Local::P( c, s * 0.5f, 0.0f, transform );
			Aurora_TouchArrowHead( list, tip, Local::V( 1.0f, 0.0f, transform ), s * 0.9f, color, thickness );
			break;
		}

		case AURORA_TOUCH_ICON_WEAP_PREV:
		{
			// Two stacked chevrons (a "«"/fast-rewind glyph), pointing left -
			// distinct from FORCE_PREV's single chevron, reads as "step
			// through a list" rather than "the one current thing".
			int j;
			for ( j = 0; j < 2; j++ )
			{
				const float dx = -s * 0.15f - (float)j * s * 0.45f;
				const ImVec2 tip = Local::P( c, dx, 0.0f, transform );
				Aurora_TouchArrowHead( list, tip, Local::V( -1.0f, 0.0f, transform ), s * 0.5f, color, thickness );
			}
			break;
		}

		case AURORA_TOUCH_ICON_WEAP_NEXT:
		{
			int j;
			for ( j = 0; j < 2; j++ )
			{
				const float dx = s * 0.15f + (float)j * s * 0.45f;
				const ImVec2 tip = Local::P( c, dx, 0.0f, transform );
				Aurora_TouchArrowHead( list, tip, Local::V( 1.0f, 0.0f, transform ), s * 0.5f, color, thickness );
			}
			break;
		}

		case AURORA_TOUCH_ICON_ROLL:
		{
			// A partial circular arc with an arrowhead at its leading end -
			// reads as "tumble", distinct from CROUCH's straight
			// arrow-onto-floor. PathArcTo/PathStroke build the arc directly
			// in absolute (window-space) angles, so unlike every other
			// shape in this switch (which routes local offsets through
			// Local::P/V before adding them to `c`) the arc's OWN start/end
			// angles are rotated by a quarter-turn per `transform` instead -
			// equivalent effect (transform is always a multiple of 90
			// degrees here), just applied to the angle parameters rather
			// than to emitted points, since PathArcTo has no "local offset"
			// form to route through Local:: at all.
			static const float angleOffset[4] = { 0.0f, -(float)M_PI * 0.5f, (float)M_PI, (float)M_PI * 0.5f };
			const float off = angleOffset[transform & 3];
			const float startAngle = -0.35f * (float)M_PI + off;
			const float endAngle = 1.15f * (float)M_PI + off;
			const float r = s * 0.65f;

			list->PathArcTo( c, r, startAngle, endAngle, 16 );
			list->PathStroke( color, ImDrawFlags_None, thickness );

			{
				const float ang = endAngle;
				const ImVec2 tip( c.x + cosf( ang ) * r, c.y + sinf( ang ) * r );
				const ImVec2 dir( -sinf( ang ), cosf( ang ) );

				Aurora_TouchArrowHead( list, tip, dir, s * 0.35f, color, thickness );
			}
			break;
		}

		default:
			break;
	}
}

/*
====================
Aurora_TouchUI_AddLabel

Text alternative to Aurora_TouchIcon, for buttons whose meaning isn't a
shape people already know (save/load/skip - port task B-003; the rest of
the overlay keeps its procedural icons, see gles3_touchui_draw.h's comment
on `label`). Uses ImGui's own default font (already loaded for
Aurora_TouchUI_EnsureInit's font-atlas requirement, see that function's
comment) at a size derived from the CAPSULE's inner height (`height`,
already ring-inset by the caller) - no extra font baking needed for four
short, all-caps words. 0.55 is picked to sit comfortably inside a capsule
built by Aurora_TouchUI_CapsuleWidthMm's 0.5-of-height/0.45-of-height-per-
side estimate (sdl_touchui.cpp) without touching the rounded ends.

Orientation: AddText() always lays glyph quads out upright in the CURRENT
draw-list space, which for every OTHER primitive in this file is window
space (see gles3_touchui_draw.h's big comment - icon shapes route every
point through Aurora_TouchUI_RotateOffset before adding them to the
already-window-space centre, so they still read as upright to the player
after the compositor's own rotation compensation, whatever `transform` is).
Text can't be built that way up front (AddText has no rotation parameter),
so instead: lay the word out centred on `c` UNROTATED first (cheapest to
size/position correctly), then rotate just the vertices AddText() appended
(a pure rigid rotation about `c`, same 4-case table as every other shape
here) - equivalent end result, one extra pass over a handful of quads.
Shared with Aurora_TouchUI_AddCapsule, which rotates the capsule's own
rectangle the identical way so the whole button turns as one rigid piece.
====================
*/
static void Aurora_TouchUI_AddLabel( ImDrawList *list, const ImVec2 &c, float height, ImU32 color, const char *label, int transform )
{
	ImFont *font = ImGui::GetFont();
	float fontSize = height * 0.55f;
	ImVec2 textSize, pos;
	int vtx0;

	if ( fontSize < 1.0f ) fontSize = 1.0f;

	textSize = font->CalcTextSizeA( fontSize, FLT_MAX, 0.0f, label );
	pos = ImVec2( c.x - textSize.x * 0.5f, c.y - textSize.y * 0.5f );

	vtx0 = list->VtxBuffer.Size;
	list->AddText( font, fontSize, pos, color, label );

	if ( transform == 0 )
	{
		return;
	}
	for ( int i = vtx0; i < list->VtxBuffer.Size; i++ )
	{
		ImDrawVert &v = list->VtxBuffer[i];
		float rx, ry;

		Aurora_TouchUI_RotateOffset( v.pos.x - c.x, v.pos.y - c.y, transform, &rx, &ry );
		v.pos.x = c.x + rx;
		v.pos.y = c.y + ry;
	}
}

/*
====================
Aurora_TouchUI_AddCapsule

SAVE/LOAD/SKIP (port task B-003 - matching the look of the reference
quake4es port, ~/Projects/aurora-ports/quake4/quake4es: a word on a
rounded-rect capsule as wide as it needs, not a circle - see
renderer/TouchOverlay.h's TouchOverlay_CapsuleWidth and
renderer/imgui/r_touch.cpp's RB_TouchOverlay_Build there). halfWidth/
halfHeight are the VISUAL-local, pre-rotation box (see
gles3_touchui_draw.h's comment on `label`) - built straight (unrotated,
long axis along local X) around `c` first, exactly like
Aurora_TouchUI_AddLabel does for the word inside it, then the handful of
vertices AddRectFilled()/AddRect() just emitted are rotated about `c` by
the same table every other shape here uses. That is the only way to get
a rotated rounded rect out of ImGui: AddRectFilled/AddRect always draw
axis-aligned in the current draw-list space, there is no "draw this
already turned" primitive - so this is the same "draw straight, then
rotate the emitted vertices" trick as the label text, just for the box
instead of the glyphs, so the two rotate together as one rigid button.
====================
*/
static void Aurora_TouchUI_AddCapsule( ImDrawList *list, const ImVec2 &c, float halfWidth, float halfHeight, ImU32 fill, ImU32 edge, ImU32 textColor, float ring, const char *label, int transform )
{
	const ImVec2 min( c.x - halfWidth + ring * 0.5f, c.y - halfHeight + ring * 0.5f );
	const ImVec2 max( c.x + halfWidth - ring * 0.5f, c.y + halfHeight - ring * 0.5f );
	const float rounding = ( max.y - min.y ) * 0.5f;
	const int vtx0 = list->VtxBuffer.Size;

	list->AddRectFilled( min, max, fill, rounding );
	list->AddRect( min, max, edge, rounding, ImDrawFlags_None, ring );

	if ( transform != 0 )
	{
		for ( int i = vtx0; i < list->VtxBuffer.Size; i++ )
		{
			ImDrawVert &v = list->VtxBuffer[i];
			float rx, ry;

			Aurora_TouchUI_RotateOffset( v.pos.x - c.x, v.pos.y - c.y, transform, &rx, &ry );
			v.pos.x = c.x + rx;
			v.pos.y = c.y + ry;
		}
	}

	Aurora_TouchUI_AddLabel( list, c, max.y - min.y, textColor, label, transform );
}

/*
====================
Aurora_TouchUI_Build
====================
*/
static void Aurora_TouchUI_Build( ImDrawList *list, const AuroraTouchDrawFrame *frame )
{
	const float alpha = frame->alpha;
	const float ring = ( 0.3f * frame->pixelsPerMm > 1.5f ) ? 0.3f * frame->pixelsPerMm : 1.5f;
	const float stroke = ( 0.45f * frame->pixelsPerMm > 2.0f ) ? 0.45f * frame->pixelsPerMm : 2.0f;

	for ( int i = 0; i < frame->numButtons; i++ )
	{
		const AuroraTouchButtonDraw &b = frame->buttons[i];
		const ImVec2 c( b.x, b.y );
		ImU32 fill = b.pressed
			? Aurora_TouchColor( 0x3B, 0x82, 0xF6, alpha * 0.8f )
			: Aurora_TouchColor( 0, 0, 0, alpha * 0.4f );
		ImU32 edge = b.pressed
			? Aurora_TouchColor( 255, 255, 255, alpha )
			: Aurora_TouchColor( 255, 255, 255, alpha * 0.7f );
		ImU32 content = Aurora_TouchColor( 255, 255, 255, alpha * 0.95f );
		const float dim = b.disabled ? 0.35f : 1.0f;

		if ( b.disabled )
		{
			fill = Aurora_TouchColor( 0, 0, 0, alpha * 0.4f * dim );
			edge = Aurora_TouchColor( 255, 255, 255, alpha * 0.7f * dim );
			content = Aurora_TouchColor( 255, 255, 255, alpha * 0.95f * dim );
		}

		if ( b.label )
		{
			Aurora_TouchUI_AddCapsule( list, c, b.halfWidth, b.halfHeight, fill, edge, content, ring, b.label, frame->transform );
			continue;
		}

		{
			const float r = ( b.radius - ring * 0.5f > 1.0f ) ? b.radius - ring * 0.5f : 1.0f;

			list->AddCircleFilled( c, r, fill );
			list->AddCircle( c, r, edge, 0, ring );
			Aurora_TouchIcon( list, b.icon, c, r * 0.6f, content, stroke, frame->transform );
		}
	}

	if ( frame->stick )
	{
		const ImVec2 base( frame->stickBaseX, frame->stickBaseY );
		const ImVec2 knob( frame->stickKnobX, frame->stickKnobY );
		const float r = frame->stickRadius;

		list->AddCircleFilled( base, r, Aurora_TouchColor( 0, 0, 0, alpha * 0.25f ) );
		list->AddCircle( base, r, Aurora_TouchColor( 255, 255, 255, alpha * 0.5f ), 0, ring );
		list->AddCircleFilled( knob, r * 0.45f, Aurora_TouchColor( 255, 255, 255, alpha * 0.45f ) );
		list->AddCircle( knob, r * 0.45f, Aurora_TouchColor( 255, 255, 255, alpha * 0.8f ), 0, ring );
	}
}

/*
====================
Aurora_TouchUI_RenderFrame

Called from gles3_touchui.cpp's Aurora_TouchUI_Draw(), default framebuffer
already bound and its viewport already covering the whole window. ImGui's
own OpenGL3 backend saves and restores every bit of GL state it touches
around ImGui_ImplOpenGL3_RenderDrawData() (see the "Backup/Restore GL state"
blocks in lib/imgui/backends/imgui_impl_opengl3.cpp) - so nothing here needs
to be undone by hand afterwards, on top of the engine's own free
re-invalidation on the next frame (see the call site's comment).
====================
*/
void Aurora_TouchUI_RenderFrame( const AuroraTouchDrawFrame *frame )
{
	if ( !frame || frame->windowWidth <= 0 || frame->windowHeight <= 0 )
	{
		return;
	}
	if ( !Aurora_TouchUI_EnsureInit() )
	{
		return;
	}

	ImGuiContext *previous = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext( aurora_touchImGuiCtx );

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2( (float)frame->windowWidth, (float)frame->windowHeight );
	io.DisplayFramebufferScale = ImVec2( 1.0f, 1.0f );
	io.DeltaTime = 1.0f / 60.0f;

	ImGui_ImplOpenGL3_NewFrame();
	ImGui::NewFrame();

	Aurora_TouchUI_Build( ImGui::GetForegroundDrawList(), frame );

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );

	ImGui::SetCurrentContext( previous );
}

void Aurora_TouchUI_ShutdownDraw( void )
{
	aurora_touchImGuiFailed = false;

	if ( !aurora_touchImGuiCtx )
	{
		return;
	}

	ImGuiContext *previous = ImGui::GetCurrentContext();

	ImGui::SetCurrentContext( aurora_touchImGuiCtx );
	ImGui_ImplOpenGL3_Shutdown();
	ImGui::DestroyContext( aurora_touchImGuiCtx );

	ImGui::SetCurrentContext( previous != aurora_touchImGuiCtx ? previous : NULL );
	aurora_touchImGuiCtx = NULL;
}
