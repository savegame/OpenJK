/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

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

// Aurora offscreen FBO module - port stage 2, gameport/docs/fbo_module.md.
//
// This is new Aurora-specific functionality, not a translation of anything
// in code/rd-vulkan (rd-vulkan has no FBO of its own - its "swapchain" is a
// different concept entirely, and its vk.fboActive/offscreenRender/
// blitEnabled fields gate unrelated quake3e-inherited behaviour, see the
// Gles3_Fbo_t comment in vk_local.h). The GLES3-side pattern this follows
// is the already-shipped Aurora port of quake3e
// (~/Projects/aurora-quake3/quake3e, code/renderergles3/gles3.c,
// gles3_fbo_* functions) - read there, nothing changed there.
//
// Why this exists: the device screen is portrait, the game is landscape-only
// (port stage 3), and physical rotation/input transform (stage 3/4) both
// need a single quad draw at the very end of the frame that can rotate and
// scale the already-rendered scene, without the scene-rendering code above
// knowing anything changed. Rendering into an offscreen color+depth/stencil
// FBO first, then blitting that FBO's color texture to the real backbuffer
// with a textured quad, is what makes that possible - glBlitFramebuffer
// cannot rotate or scale (fbo_module.md, "Вывод на экран").
//
// Stage 2 scope: build the FBO, render the whole frame into it, blit it to
// the screen with an identity quad (GLES3_FBO_TRANSFORM_NORMAL, scale 1.0).
// The rotation/scale *machinery* (cached matrices, get/set API) is built now
// per fbo_module.md's "закладывайся" instruction, but stage 3 is the one
// that will start calling gles3_fbo_set_rotation with a real value.

#include "tr_local.h"
#include "gles3_local.h"

// ---------------------------------------------------------------------------
// blit-quad shader (compiled once, reused every frame - fbo_module.md:
// "шейдеры компилируются один раз и переиспользуются")
// ---------------------------------------------------------------------------

// No vertex buffer: the quad's 4 corners come from gl_VertexID (core in
// GLSL ES 3.00), matching the reference port's gles3_draw_fullscreen_quad -
// one less GL object to own/leak across R_Init/RE_Shutdown cycles. The
// rotation matrix touches ONLY the position, never the UV, exactly as
// fbo_module.md's "Вывод на экран" section requires.
static const char *const VS_FBO_BLIT =
	"precision highp float;\n"
	"uniform mat2 u_Rotation;\n"
	"out vec2 v_uv;\n"
	"const vec2 kPos[4] = vec2[4](\n"
	"	vec2(-1.0, 1.0),\n"
	"	vec2(-1.0,-1.0),\n"
	"	vec2( 1.0, 1.0),\n"
	"	vec2( 1.0,-1.0)\n"
	");\n"
	"const vec2 kUV[4] = vec2[4](\n"
	"	vec2(0.0, 1.0),\n"
	"	vec2(0.0, 0.0),\n"
	"	vec2(1.0, 1.0),\n"
	"	vec2(1.0, 0.0)\n"
	");\n"
	"void main() {\n"
	"	gl_Position = vec4( u_Rotation * kPos[gl_VertexID], 0.0, 1.0 );\n"
	"	v_uv = kUV[gl_VertexID];\n"
	"}\n";

// Pure passthrough on purpose: gamma/intensity are already baked into the
// scene by the existing (hardware-gamma-ramp) pipeline, same as before this
// FBO bounce was added, so this stage must not touch color at all - any
// change here would be a visible regression against the accepted G2b
// reference screenshots. Software gamma-in-the-blit (dither/greyscale/
// obScale) is deferred, see the removed vk.fboActive discussion in
// vk_local.h.
static const char *const FS_FBO_BLIT =
	"precision mediump float;\n"
	"uniform sampler2D u_Texture0;\n"
	"in vec2 v_uv;\n"
	"out vec4 out_color;\n"
	"void main() {\n"
	"	out_color = texture( u_Texture0, v_uv );\n"
	"}\n";

// Column-major mat2 values (glUniformMatrix2fv layout: col0.x, col0.y,
// col1.x, col1.y) per GLES3_FBO_TRANSFORM_*, precomputed once so stage 3
// never recomputes anything per-frame (fbo_module.md: "кэшировать всё, что
// можно").
//
// Direction: wl_output_transform counts COUNTER-clockwise, and fbo_module.md
// ("Вывод на экран") requires the visible rotation of the content to equal
// the value handed to wl_surface.set_buffer_transform, because the same enum
// value goes to both. Since only the quad's vertex POSITIONS are rotated
// (never the UVs), the content's visible rotation is exactly this matrix's
// rotation, so these must be genuine CCW rotations of the NDC position:
//
//   R(t) * (x, y) = ( x*cos t - y*sin t, x*sin t + y*cos t )
//     90  -> (-y,  x)   col0 = R*(1,0) = ( 0, 1), col1 = R*(0,1) = (-1, 0)
//    180  -> (-x, -y)   col0 = (-1, 0),           col1 = ( 0,-1)
//    270  -> ( y, -x)   col0 = ( 0,-1),           col1 = ( 1, 0)
//
// These were previously copied verbatim from the Aurora quake3e port
// (code/renderergles3/gles3.c gles3_fbo_init_rotation_mats), whose 90 and
// 270 entries are labelled CCW but hold each other's values - i.e. its "90"
// is really 90 CW. That port compensates elsewhere, with a constant
// +2 (mod 4) index offset applied before indexing this table
// (QUAD_ROTATE_OFFSET, code/sdl/sdl_input.c): for the only two transforms a
// portrait panel ever uses, 1 and 3, (t + 2) % 4 into a table with 1 and 3
// swapped lands on exactly the true-CCW matrix for t. Net behaviour there is
// therefore identical to the plain table below - the swap and the offset
// cancelled. Copying the table without the offset did not, which is what put
// the content on screen 180 degrees off (rotating CW where the compositor,
// told transform=90, expects CCW). Fixed here at the source instead of
// importing the offset: this port has no hidden extra flip (its identity/
// NORMAL blit was verified upright in the accepted G2b screenshots), so
// transform -> matrix is direct, with nothing to cancel.
static void gles3_fbo_init_rotation_mats( void )
{
	vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_NORMAL][0] =  1.0f; vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_NORMAL][1] =  0.0f;
	vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_NORMAL][2] =  0.0f; vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_NORMAL][3] =  1.0f;

	vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_90][0] =  0.0f; vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_90][1] =  1.0f;
	vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_90][2] = -1.0f; vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_90][3] =  0.0f;

	vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_180][0] = -1.0f; vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_180][1] =  0.0f;
	vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_180][2] =  0.0f; vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_180][3] = -1.0f;

	vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_270][0] =  0.0f; vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_270][1] = -1.0f;
	vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_270][2] =  1.0f; vk.fbo.rotationMats[GLES3_FBO_TRANSFORM_270][3] =  0.0f;
}

// Port stage 3: the engine-facing "output size" (== the FBO's own size) for a
// given REAL window size. This game is landscape-only - its 3D projection and
// its whole 2D/UI layer are built from glConfig.vidWidth/vidHeight, and a
// portrait pair there yields a squashed scene and an off-screen menu, not a
// rotated game - so on a portrait window the output size is the transpose of
// the window, never the window itself (fbo_module.md, "Размер окна и буфера
// при повороте"; the same "the buffer is ALWAYS landscape" rule the Aurora
// quake3e port's gles3_fbo_compute_size settled on). A landscape window
// (tablet/monitor/TV, or a desktop host build) needs no swap.
//
// Orienting by the window's own shape rather than by the current rotation is
// deliberate: the two disagree the moment an external display is involved
// (panel type flips to landscape, so the transform goes to 0, while the
// window is still portrait), and it is the shape, not the transform, that
// decides what the engine can render into. The transform then decides only
// how that buffer is drawn on screen.
void gles3_fbo_output_size_for_window( uint32_t winW, uint32_t winH, uint32_t *outW, uint32_t *outH )
{
#ifdef AURORA
	if ( winH > winW ) {
		if ( outW ) *outW = winH;
		if ( outH ) *outH = winW;
		return;
	}
#endif
	if ( outW ) *outW = winW;
	if ( outH ) *outH = winH;
}

void gles3_fbo_init_program( void )
{
	vk.fbo.blitProgram = gles3_link_program( VS_FBO_BLIT, NULL, FS_FBO_BLIT, NULL );
	vk.fbo.blitLoc_rotation = glGetUniformLocation( vk.fbo.blitProgram, "u_Rotation" );
	vk.fbo.blitLoc_texture0 = glGetUniformLocation( vk.fbo.blitProgram, "u_Texture0" );

	gles3_fbo_init_rotation_mats();
	vk.fbo.scale = 1.0f;

	// vk.fbo.transform is deliberately NOT reset here (port stage 3, remark
	// 1). This runs from vk_initialize(), which is re-entered on every FBO
	// rebuild - including SP's "dead window" contract (RE_Shutdown then a
	// second R_Init once a map starts loading) and any live resize - so
	// resetting it to NORMAL here silently threw away the rotation that
	// vk_create_window and/or a "gles3_set_rotation" from
	// shared/sdl/sdl_input.cpp had already established, leaving the content
	// portrait while the compositor had already been told the buffer was
	// landscape. The current transform belongs to the window/orientation,
	// not to this GL program, and vk_create_window seeds it (from the
	// "cl_auroraInitTransform" cvar - see the comment there) before the
	// first vk_initialize() ever runs.
	if ( vk.fbo.transform < GLES3_FBO_TRANSFORM_NORMAL || vk.fbo.transform > GLES3_FBO_TRANSFORM_270 ) {
		vk.fbo.transform = GLES3_FBO_TRANSFORM_NORMAL;
	}
}

void gles3_fbo_destroy_program( void )
{
	if ( vk.fbo.blitProgram ) {
		glDeleteProgram( vk.fbo.blitProgram );
		vk.fbo.blitProgram = 0;
	}
}

// ---------------------------------------------------------------------------
// FBO object (color texture + depth/stencil renderbuffer + framebuffer)
// ---------------------------------------------------------------------------

static const char *gles3_fbo_status_string( GLenum status )
{
	switch ( status ) {
		case GL_FRAMEBUFFER_COMPLETE:						return "GL_FRAMEBUFFER_COMPLETE";
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:			return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:	return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
		case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:			return "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
		case GL_FRAMEBUFFER_UNSUPPORTED:					return "GL_FRAMEBUFFER_UNSUPPORTED";
		case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:		return "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE";
		default:											return "unknown status";
	}
}

void gles3_fbo_destroy( void )
{
	if ( vk.fbo.framebuffer ) {
		glDeleteFramebuffers( 1, &vk.fbo.framebuffer );
		vk.fbo.framebuffer = 0;
	}
	if ( vk.fbo.colorTexture ) {
		glDeleteTextures( 1, &vk.fbo.colorTexture );
		vk.fbo.colorTexture = 0;
	}
	if ( vk.fbo.depthStencilBuffer ) {
		glDeleteRenderbuffers( 1, &vk.fbo.depthStencilBuffer );
		vk.fbo.depthStencilBuffer = 0;
	}
	vk.fbo.active = qfalse;
	vk.fbo.width = 0;
	vk.fbo.height = 0;
}

// Builds vk.fbo sized width x height: a GL_RGBA/GL_UNSIGNED_BYTE color
// texture plus, preferably, a single combined GL_DEPTH24_STENCIL8
// renderbuffer attached to both GL_DEPTH_ATTACHMENT and GL_STENCIL_ATTACHMENT
// (fbo_module.md's verified-working format). On failure it retries
// depth-only (GL_DEPTH_COMPONENT16, no stencil - shadow volumes lose their
// stencil test but the world still renders) rather than the explicitly
// disallowed separate DEPTH_COMPONENT16 + STENCIL_INDEX8 pair, which
// fbo_module.md notes drivers may reject outright.
//
// fbo_module.md asks to gate the combined format on the
// GL_OES_packed_depth_stencil extension string. That token is an ES2-era
// name; this backend requires an ES 3.0+ context unconditionally
// (vk_create_window: majorVersion=3, minorVersion=0), and GL_DEPTH24_STENCIL8
// is a REQUIRED core renderbuffer format from ES 3.0 onward (some ES3
// drivers simply stop listing the legacy extension name once a feature is
// core, which would make a string-only check false-negative on a driver
// that actually supports it). glCheckFramebufferStatus below is the
// authority fbo_module.md itself mandates as the final check either way, so
// it alone drives the fallback - the practical effect is identical to what
// the doc asks for, just triggered by the real capability instead of a
// string that ES3 drivers are not required to keep publishing.
void gles3_fbo_create( uint32_t width, uint32_t height )
{
	GLenum status;

	gles3_fbo_destroy();

	if ( width == 0 || height == 0 ) {
		return;
	}

	glGenTextures( 1, &vk.fbo.colorTexture );
	glBindTexture( GL_TEXTURE_2D, vk.fbo.colorTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, G3_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, G3_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &vk.fbo.framebuffer );
	glBindFramebuffer( GL_FRAMEBUFFER, vk.fbo.framebuffer );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vk.fbo.colorTexture, 0 );

	glGenRenderbuffers( 1, &vk.fbo.depthStencilBuffer );
	glBindRenderbuffer( GL_RENDERBUFFER, vk.fbo.depthStencilBuffer );
	glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, vk.fbo.depthStencilBuffer );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, vk.fbo.depthStencilBuffer );
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );

	status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	vk.fbo.hasStencil = qtrue;

	if ( status != GL_FRAMEBUFFER_COMPLETE ) {
		ri.Printf( PRINT_WARNING, "gles3_fbo: GL_DEPTH24_STENCIL8 attachment failed (%s), retrying depth-only\n",
			gles3_fbo_status_string( status ) );

		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0 );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0 );
		glDeleteRenderbuffers( 1, &vk.fbo.depthStencilBuffer );

		glGenRenderbuffers( 1, &vk.fbo.depthStencilBuffer );
		glBindRenderbuffer( GL_RENDERBUFFER, vk.fbo.depthStencilBuffer );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, vk.fbo.depthStencilBuffer );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );

		vk.fbo.hasStencil = qfalse;
		status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	}

	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	ri.Printf( PRINT_ALL, "gles3_fbo: %ux%u, %s, status %s\n", width, height,
		vk.fbo.hasStencil ? "GL_DEPTH24_STENCIL8" : "GL_DEPTH_COMPONENT16 (no stencil)",
		gles3_fbo_status_string( status ) );

	if ( status != GL_FRAMEBUFFER_COMPLETE ) {
		// Never fatal: fall back to rendering straight into the default
		// framebuffer, i.e. stage 1/G2b behaviour, rather than taking the
		// game down over a rotation-readiness feature.
		ri.Printf( PRINT_WARNING, "gles3_fbo: framebuffer incomplete, disabling offscreen rendering\n" );
		gles3_fbo_destroy();
		return;
	}

	vk.fbo.width = width;
	vk.fbo.height = height;
	vk.fbo.active = qtrue;
}

qboolean gles3_fbo_resize( uint32_t width, uint32_t height )
{
	if ( width == vk.fbo.width && height == vk.fbo.height ) {
		return qfalse;
	}

	gles3_fbo_create( width, height );
	return qtrue;
}

// Port stage 3 (gameport/docs/fbo_module.md, "Размер окна и буфера при
// повороте"): the full handler for a REAL window/buffer resize, replacing
// the vid_restart that used to run here (see shared/sdl/sdl_input.cpp
// history - commit 6b6a32a). A resize is just a resized render target: the
// FBO's color/depth-stencil storage is rebuilt at the new size and the
// engine's own idea of "output size" is updated so it recomputes its view/
// viewport math, all inside the current frame - no CL_ShutdownRef/CL_InitRef
// cascade, no shader/image reload, no SDL window recreation. This is never
// called on a pure device rotation: rotation only ever changes
// gles3_fbo_set_rotation's transform and the Wayland buffer transform
// (sdl_input.cpp), the buffer size itself never changes for that case.
// width/height are the REAL new window/surface size, exactly as SDL reported
// it (shared/sdl/sdl_input.cpp's SDL_WINDOWEVENT_SIZE_CHANGED -> the
// "gles3_resize" console command). The engine-facing output size and the
// FBO's own size are derived from it here, not passed in, so that the single
// landscape-only rule lives in one place
// (gles3_fbo_output_size_for_window above).
void gles3_fbo_handle_resize( uint32_t width, uint32_t height )
{
	uint32_t outW, outH;

	if ( width == 0 || height == 0 ) {
		return;
	}
	if ( width == vk.windowWidth && height == vk.windowHeight ) {
		return;
	}

	vk.windowWidth = width;
	vk.windowHeight = height;

	gles3_fbo_output_size_for_window( width, height, &outW, &outH );

	// Everything the engine measures its output by - aspect ratio and 3D
	// projection (tr_main.cpp), the 640x480-virtual 2D/UI mapping
	// (SCR_AdjustFrom640), console reflow - reads glConfig.vidWidth/
	// vidHeight, so updating them here IS the "recompute the aspect ratio
	// for the new FBO size" step: they carry the FBO's landscape dimensions,
	// never the portrait window's (port stage 3, remark 3).
	glConfig.vidWidth = (int)outW;
	glConfig.vidHeight = (int)outH;

	// The rd-gles3 "output size" used every frame by gles3_get_viewport_rect
	// (gles3_frame.cpp) for the 2D viewport/scissor - renderScaleX/Y are left
	// alone (r_renderScale is CVAR_LATCH, not something a live resize should
	// touch; vk_initialize always leaves them at 1.0 in this backend too).
	vk.renderWidth = outW;
	vk.renderHeight = outH;

	gls.windowWidth = outW;
	gls.windowHeight = outH;
	gls.captureWidth = outW;
	gls.captureHeight = outH;

	gles3_fbo_resize( outW, outH );

	// Re-establish the default-framebuffer viewport/scissor immediately
	// rather than waiting for the next draw call to notice: harmless to do
	// with raw GL here even though it bypasses the gles3_state cache
	// (gles3_frame.cpp), because vk_begin_frame() unconditionally calls
	// gles3_state_cache_invalidate() before the next frame's first draw, so
	// the cache resyncs for free either way (same reasoning as
	// gles3_fbo_blit_to_screen() above). The default framebuffer is the real
	// window, so this one takes the window size, not the output size.
	glViewport( 0, 0, width, height );
	glScissor( 0, 0, width, height );

	ri.Printf( PRINT_ALL, "gles3_fbo: window resized to %ux%u -> output/FBO %ux%u\n",
		width, height, outW, outH );
}

void gles3_fbo_bind( void )
{
	glBindFramebuffer( GL_FRAMEBUFFER, vk.fbo.active ? vk.fbo.framebuffer : 0 );
}

// Measured on device (Mali-G57 MC2): the offscreen bounce alone dropped the
// uncapped fps ceiling on academy1 from ~222 to ~178 (com_maxfps 333,
// see work/stage_01_build.md) - the classic tile-based-GPU cost of writing
// a whole extra depth/stencil attachment back to system memory at the end
// of every render pass, on top of the one the default framebuffer already
// pays at swap. We never read vk.fbo's depth/stencil back (only its color
// texture, in the blit quad below) and never carry it to the next frame
// (glClear() re-clears it every frame, see vk_clear_depthstencil_attachments
// in gles3_frame.cpp) - textbook glInvalidateFramebuffer material, exactly
// what fbo_module.md's performance note names. Must run while vk.fbo's
// framebuffer is still bound (the driver hint applies to whatever's
// currently bound), i.e. before the caller switches to the default
// framebuffer for the blit.
void gles3_fbo_invalidate_depth_stencil( void )
{
	GLenum attachments[2];
	GLsizei count = 0;

	if ( !vk.fbo.active ) {
		return;
	}

	attachments[count++] = GL_DEPTH_ATTACHMENT;
	if ( vk.fbo.hasStencil ) {
		attachments[count++] = GL_STENCIL_ATTACHMENT;
	}
	glInvalidateFramebuffer( GL_FRAMEBUFFER, count, attachments );
}

GLuint gles3_fbo_get_color_texture( void )
{
	return vk.fbo.colorTexture;
}

void gles3_fbo_get_size( uint32_t *width, uint32_t *height )
{
	if ( width )  *width  = vk.fbo.width;
	if ( height ) *height = vk.fbo.height;
}

void gles3_fbo_set_scale( float scale )
{
	vk.fbo.scale = ( scale > 0.0f ) ? scale : 1.0f;
}

float gles3_fbo_get_scale( void )
{
	return vk.fbo.scale;
}

void gles3_fbo_set_rotation( int transform )
{
	if ( transform < GLES3_FBO_TRANSFORM_NORMAL || transform > GLES3_FBO_TRANSFORM_270 ) {
		transform = GLES3_FBO_TRANSFORM_NORMAL;
	}
	vk.fbo.transform = transform;
}

int gles3_fbo_get_rotation( void )
{
	return vk.fbo.transform;
}

// Draws vk.fbo's color texture as a full-viewport quad into whichever
// framebuffer is bound when this is called (the caller, vk_present_frame in
// gles3_frame.cpp, binds the default framebuffer first) and leaves that
// framebuffer bound. Only glDrawArrays touches the GPU - no glBlitFramebuffer
// (fbo_module.md: blit has neither rotation nor scaling), no per-frame vertex
// upload (the quad has no vertex buffer at all, see VS_FBO_BLIT above), no
// per-frame branching beyond the qboolean guard itself.
//
// The GL state this sets (program, texture unit 0, depth/stencil/blend/cull/
// scissor/color-mask) is applied with raw GL calls, bypassing
// gles3_set_state's delta cache in gles3_frame.cpp on purpose - resyncing
// that cache here would cost more than it saves for a once-per-frame call.
// It is safe to leave the cache stale: vk_begin_frame() unconditionally
// calls gles3_state_cache_invalidate() before the next frame's first draw
// (gles3_frame.cpp), and vk_bind_pipeline() always issues a fresh
// glUseProgram() regardless of what was last bound (gles3_geometry.cpp) -
// so both resync for free on the very next pipeline bind.
void gles3_fbo_blit_to_screen( void )
{
	if ( !vk.fbo.active ) {
		return;
	}

	glDisable( GL_DEPTH_TEST );
	glDepthMask( GL_FALSE );
	glDisable( GL_STENCIL_TEST );
	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );
	glDisable( GL_SCISSOR_TEST );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

	// The REAL window size: this draws into the default framebuffer, which
	// glConfig.vidWidth/vidHeight no longer describes once the FBO is the
	// landscape transpose of a portrait window (port stage 3, remark 3 - see
	// the comment on vk.windowWidth in vk_local.h). The quad itself is full
	// NDC and a 90/270 rotation maps that square onto itself, so it still
	// covers the whole window; because the FBO is the exact transpose of the
	// window, the rotated texture lands 1:1 on the window pixels and the
	// aspect ratio comes out undistorted with no letterbox math needed.
	glViewport( 0, 0, vk.windowWidth, vk.windowHeight );

	glUseProgram( vk.fbo.blitProgram );

	// vk_select_texture (not a raw glActiveTexture) so vk.ctmu - the cache
	// vk_select_texture itself uses to skip redundant glActiveTexture calls,
	// gles3_geometry.cpp, persists across frames rather than being reset
	// each one - stays in sync with the unit this leaves active for real.
	vk_select_texture( 0 );
	glBindTexture( GL_TEXTURE_2D, vk.fbo.colorTexture );
	if ( vk.fbo.blitLoc_texture0 >= 0 ) {
		glUniform1i( vk.fbo.blitLoc_texture0, 0 );
	}
	if ( vk.fbo.blitLoc_rotation >= 0 ) {
		glUniformMatrix2fv( vk.fbo.blitLoc_rotation, 1, GL_FALSE, vk.fbo.rotationMats[vk.fbo.transform] );
	}

	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
}

// ---------------------------------------------------------------------------
// port stage 3 console commands - registered in tr_init.cpp's commands[]
// table, see the comment on their declaration in vk_local.h
// ---------------------------------------------------------------------------

void GLES3_Resize_f( void )
{
	if ( ri.Cmd_Argc() != 3 ) {
		ri.Printf( PRINT_ALL, "usage: gles3_resize <width> <height>\n" );
		return;
	}
	gles3_fbo_handle_resize( (uint32_t)atoi( ri.Cmd_Argv( 1 ) ), (uint32_t)atoi( ri.Cmd_Argv( 2 ) ) );
}

// transform is a raw GLES3_FBO_TRANSFORM_*/wl_output_transform value (0-3) -
// shared/sdl/sdl_input.cpp already resolved system orientation + panel type
// down to that single number (gameport/docs/fbo_module.md, "Маппинг
// ориентаций под ориентацию игры"); this command just hands it to
// gles3_fbo_set_rotation, which is the only place that picks the active
// rotation matrix (never in gles3_fbo_blit_to_screen's per-frame draw).
void GLES3_SetRotation_f( void )
{
	if ( ri.Cmd_Argc() != 2 ) {
		ri.Printf( PRINT_ALL, "usage: gles3_set_rotation <transform 0-3>\n" );
		return;
	}
	gles3_fbo_set_rotation( atoi( ri.Cmd_Argv( 1 ) ) );
}
