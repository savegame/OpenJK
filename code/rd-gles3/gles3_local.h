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

// Internal helpers shared between the gles3_*.cpp backend units
// (not part of the vk_* frontend-facing API).

#pragma once

// image_t.handle is a VkImage (opaque pointer typedef) in the shared
// tr_local.h; the gles3 backend stores the GL texture name in it via an
// uintptr_t round-trip.
#define G3_IMG_H(h)			((GLuint)(uintptr_t)(h))

// viewport/scissor in GL coordinates (origin bottom-left)
void	gles3_get_viewport_rect( int *x, int *y, int *w, int *h );
void	gles3_get_scissor_rect( int *x, int *y, int *w, int *h );

// state cache: apply GLS_* state bits + cull mode + polygon offset of a
// pipeline def, delta-cached against the currently bound GL state
void	gles3_set_state( uint32_t state_bits, cullType_t face_culling, qboolean polygon_offset );
void	gles3_state_cache_invalidate( void );

// pipeline state that Vulkan bakes into the pipeline object but GL keeps as
// sticky global state - replayed by vk_bind_pipeline, dropped by the
// invalidate above
void	gles3_set_shadow_phase( Vk_Shadow_Phase phase, cullType_t face_culling );
void	gles3_set_colormask( qboolean write_color );
void	gles3_attribs_invalidate( void );

// depth range -> viewport/depth range update (cached per command buffer)
void	gles3_update_depth_range( Vk_Depth_Range depth_range );

// geometry streaming: upload staged host data to the VBO/IBO
void	gles3_flush_geometry( void );
void	gles3_index_stage_destroy( void );
void	gles3_resize_geometry_buffers( void );

// uniform staging (gles3_geometry.cpp)
void	gles3_uniform_reset( void );
void	gles3_uniform_shutdown( void );

// upload vk.uniform + MVP to the active program (called at draw time)
void	gles3_apply_uniforms( void );

// shader compile/link helpers (gles3_shaders.cpp), reused by gles3_fbo.cpp
// for the blit-quad program instead of duplicating them
GLuint	gles3_compile_shader( GLenum type, const char *src, const char *defines );
GLuint	gles3_link_program( const char *vs_src, const char *vs_defines, const char *fs_src, const char *fs_defines );

// ---------------------------------------------------------------------------
// Aurora offscreen FBO (gles3_fbo.cpp) - port stage 2,
// gameport/docs/fbo_module.md. Renders the frame into vk.fbo instead of the
// default framebuffer, then blits it to the screen with a single textured
// quad so a later stage can rotate/scale that quad without touching scene
// rendering. See the vk.fbo (Gles3_Fbo_t) comment in vk_local.h for why this
// is independent from the legacy vk.fboActive/offscreenRender/blitEnabled.
// ---------------------------------------------------------------------------

// (re)creates vk.fbo sized to width x height; on failure leaves vk.fbo.active
// qfalse and the caller renders straight into the default framebuffer, same
// as before this module existed - never a fatal error
void		gles3_fbo_create( uint32_t width, uint32_t height );
void		gles3_fbo_destroy( void );
// gles3_fbo_create() if width/height actually changed, qtrue if it did.
// Called by gles3_fbo_handle_resize() below on a real buffer resize; a no-op
// on pure rotation (fbo_module.md: buffer size never changes on rotation).
qboolean	gles3_fbo_resize( uint32_t width, uint32_t height );
// Port stage 3: full handler for a REAL window/buffer resize (never called
// on rotation - see gles3_fbo_set_rotation for that path). Updates the
// engine's own output dimensions (glConfig.vidWidth/Height, vk.renderWidth/
// Height, gls.window/captureWidth/Height) and rebuilds vk.fbo at the new
// size - a cheap, single-frame operation, no vid_restart / asset reload.
// Triggered from shared/sdl/sdl_input.cpp via the "gles3_resize <w> <h>"
// console command (sdl_input.cpp cannot call into the renderer directly -
// it is compiled into the client executable, the renderer is a separate
// dynamically loaded module, see tr_init.cpp's commands[] table). A no-op
// if width/height are 0 or already match the current size.
void		gles3_fbo_handle_resize( uint32_t width, uint32_t height );
// Port stage 3: the single place the landscape-only rule lives. Maps a REAL
// window size to the engine-facing output size, which is also the FBO's own
// size: the transpose of a portrait window (this game cannot render portrait
// at all), the window size unchanged otherwise. Used by vk_create_window at
// startup and by gles3_fbo_handle_resize on a live resize, so both derive the
// same thing the same way.
void		gles3_fbo_output_size_for_window( uint32_t winW, uint32_t winH, uint32_t *outW, uint32_t *outH );
// called once at startup to build the blit shader/program (persists across
// gles3_fbo_create/destroy calls, which only touch the FBO's own GL objects)
void		gles3_fbo_init_program( void );
void		gles3_fbo_destroy_program( void );

// binds vk.fbo as the active render target (no-op, i.e. default framebuffer,
// if vk.fbo.active is false)
void		gles3_fbo_bind( void );
// tells the driver vk.fbo's depth/stencil attachment content is no longer
// needed (the 3D pass is done with it and the blit-quad never reads it) -
// must be called while vk.fbo.framebuffer is still bound, i.e. before
// gles3_fbo_blit_to_screen()'s caller switches to the default framebuffer.
// A no-op if vk.fbo.active is false. See gles3_fbo.cpp for why this matters
// on tile-based mobile GPUs.
void		gles3_fbo_invalidate_depth_stencil( void );
// draws the FBO's color texture into the currently bound framebuffer as a
// full-viewport quad (rotated per gles3_fbo_set_rotation), then binds the
// default framebuffer back; no-op if vk.fbo.active is false
void		gles3_fbo_blit_to_screen( void );

GLuint		gles3_fbo_get_color_texture( void );
void		gles3_fbo_get_size( uint32_t *width, uint32_t *height );

void		gles3_fbo_set_scale( float scale );
float		gles3_fbo_get_scale( void );

void		gles3_fbo_set_rotation( int transform );
int			gles3_fbo_get_rotation( void );
