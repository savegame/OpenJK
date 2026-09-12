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

// GLES3 window creation, initialization, shutdown, clear color for rd-gles3.
// Replaces rd-vulkan vk_init.cpp / vk_instance.cpp.

#include "tr_local.h"
#include "gles3_local.h"
#include "../rd-common/tr_common.h"

void vk_set_clearcolor( void ) {
	vec4_t clr;

	clr[0] = 0.0f; clr[1] = 0.0f; clr[2] = 0.0f; clr[3] = 1.0f;

	if ( r_fastsky->integer )
	{
		vec4_t *out;

		switch( r_fastsky->integer ){
			case 1: out = &colorBlack; break;
			case 2: out = &colorRed; break;
			case 3: out = &colorGreen; break;
			case 4: out = &colorBlue; break;
			case 5: out = &colorYellow; break;
			case 6: out = &colorOrange; break;
			case 7: out = &colorMagenta; break;
			case 8: out = &colorCyan; break;
			case 9: out = &colorWhite; break;
			case 10: out = &colorLtGrey; break;
			case 11: out = &colorMdGrey; break;
			case 12: out = &colorDkGrey; break;
			case 13: out = &colorLtBlue; break;
			case 14: out = &colorDkBlue; break;
			default: out = &colorBlack;
		}

		Com_Memcpy(  tr.clearColor, *out, sizeof( vec4_t ) );
		return;
	}

	if ( tr.world && tr.world->globalFog != -1 )
	{
		const fog_t	*fog = &tr.world->fogs[tr.world->globalFog];
		Com_Memcpy(clr, (float*)fog->color, sizeof(vec3_t));
	}

	Com_Memcpy( tr.clearColor, clr, sizeof( vec4_t ) );
}

static void gles3_destroy_geometry_buffers( void )
{
	gles3_index_stage_destroy();

	if ( vk.vertex_buffer ) {
		glDeleteBuffers( 1, &vk.vertex_buffer );
		vk.vertex_buffer = 0;
	}
	if ( vk.index_buffer ) {
		glDeleteBuffers( 1, &vk.index_buffer );
		vk.index_buffer = 0;
	}
	if ( vk.geometry_buffer ) {
		free( vk.geometry_buffer );
		vk.geometry_buffer = NULL;
	}
	vk.geometry_buffer_size = 0;
}

static void gles3_create_geometry_buffers( uint32_t vertex_size, uint32_t index_size )
{
	gles3_destroy_geometry_buffers();

	vk.geometry_buffer_size = vertex_size;
	vk.index_buffer_size = index_size;

	vk.geometry_buffer = (byte*)malloc( vk.geometry_buffer_size );
	if ( vk.geometry_buffer == NULL ) {
		ri.Error( ERR_FATAL, "gles3: can't allocate geometry host buffer" );
	}

	glGenBuffers( 1, &vk.vertex_buffer );
	glBindBuffer( GL_ARRAY_BUFFER, vk.vertex_buffer );
	glBufferData( GL_ARRAY_BUFFER, vk.geometry_buffer_size, NULL, GL_STREAM_DRAW );

	glGenBuffers( 1, &vk.index_buffer );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, vk.index_buffer );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, vk.index_buffer_size, NULL, GL_STREAM_DRAW );

	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );

	gles3_geometry_buffer_reset();
}

// Counterpart of rd-vulkan vk_resize_geometry_buffer (vk_frame.cpp:1296):
// a frame that overflowed the streaming buffers records the size it needed
// and skips its draws; the buffers are grown here, at end of that frame.
void gles3_resize_geometry_buffers( void )
{
	uint32_t vertex_size = vk.geometry_buffer_size_new ? vk.geometry_buffer_size_new : vk.geometry_buffer_size;
	uint32_t index_size = vk.index_buffer_size_new ? vk.index_buffer_size_new : vk.index_buffer_size;

	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );

	gles3_create_geometry_buffers( vertex_size, index_size );

	ri.Printf( PRINT_DEVELOPER, "...geometry buffers resized to %iK vertex / %iK index\n",
		(int)( vk.geometry_buffer_size / 1024 ), (int)( vk.index_buffer_size / 1024 ) );
}

static void vk_render_splash( void )
{
	// get something on screen asap: clear to the console black
	glViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
	ri.WIN_Present( &window );
}

void vk_initialize( void )
{
	if ( vk.active ) {
		// idempotent re-init (SP dead-window contract: R_Init after RE_Shutdown)
		gles3_destroy_programs();
		gles3_fbo_destroy_program();
		gles3_fbo_destroy();
	}

	// GL strings / config
	{
		const char *str;

		str = (const char *)glGetString( GL_VENDOR );
		Q_strncpyz( vk.vendor_string, str ? str : "unknown", sizeof( vk.vendor_string ) );

		str = (const char *)glGetString( GL_RENDERER );
		Q_strncpyz( vk.renderer_string, str ? str : "unknown", sizeof( vk.renderer_string ) );

		str = (const char *)glGetString( GL_VERSION );
		Q_strncpyz( vk.version_string, str ? str : "unknown", sizeof( vk.version_string ) );

		str = (const char *)glGetString( GL_EXTENSIONS );
		Q_strncpyz( vk.device_extensions_string, str ? str : "", sizeof( vk.device_extensions_string ) );
	}

	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", vk.renderer_string );
	ri.Printf( PRINT_ALL, "GL_VERSION: %s\n", vk.version_string );

	// EGL_SWAP_BEHAVIOR is intentionally left at its default (EGL_BUFFER_DESTROYED)
	// here. It used to be forced to EGL_BUFFER_PRESERVED unconditionally, on the
	// theory that screenshot/video readback (vk_read_pixels) needed to see the
	// *presented* (post-swap) frame. EGL_BUFFER_PRESERVED is a known performance
	// trap on tile-based mobile GPUs: it forces the driver to load the previous
	// frame's backbuffer contents into tile memory before rendering starts,
	// instead of letting the frame begin from a clean tile (the whole point of
	// tile-based rendering) - paid on every single frame.
	// Checked against the actual call order in RB_SwapBuffers (tr_backend.cpp):
	// the readback runs, and backEnd.screenshotMask is cleared, *before*
	// vk_present_frame()'s WIN_Present (the actual eglSwapBuffers) - so
	// RB_ReadPixels always reads the still-current, not-yet-swapped backbuffer
	// of the frame that was just rendered. EGL_BUFFER_PRESERVED never affected
	// what a screenshot captured here; removing it is a pure win.

	glConfig.deviceSupportsGamma = qfalse;
	glConfig.doStencilShadowsInOneDrawcall = qtrue; // glStencilOpSeparate is ES3 core

	{
		GLint maxTextureSize = 0;
		GLint maxUnits = 0;
		glGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxTextureSize );
		glGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS, &maxUnits );
		glConfig.maxTextureSize = maxTextureSize;
		if ( glConfig.maxTextureSize <= 0 )
			glConfig.maxTextureSize = 1024;
		if ( glConfig.maxTextureSize > MAX_TEXTURE_SIZE )
			glConfig.maxTextureSize = MAX_TEXTURE_SIZE;
		glConfig.maxActiveTextures = maxUnits > MAX_TEXTURE_UNITS ? MAX_TEXTURE_UNITS : maxUnits;
	}
	glConfig.maxTextureFilterAnisotropy = 0;
	vk.maxAnisotropy = 0;
	ri.Cvar_Set( "r_ext_texture_filter_anisotropic_avail", "0" );

	// uniform emulation sizes (offsets returned by vk_append_uniform)
	vk.uniform_item_size			= PAD( sizeof(vkUniform_t),			(size_t)64 );
	vk.uniform_camera_item_size		= PAD( sizeof(vkUniformCamera_t),	(size_t)64 );
	vk.uniform_entity_item_size		= PAD( sizeof(vkUniformEntity_t),	(size_t)64 );
	vk.uniform_fogs_item_size		= PAD( sizeof(vkUniformFog_t),		(size_t)64 );

	vk.maxBoundDescriptorSets = 4;

	vk.cmd_index = 0;
	vk.cmd = &vk.tess[vk.cmd_index];
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	vk.cmd->waitForFence = qfalse;

	vk.renderWidth = glConfig.vidWidth;
	vk.renderHeight = glConfig.vidHeight;
	vk.renderScaleX = 1.0f;
	vk.renderScaleY = 1.0f;
	vk.xscale2D = 1.0f;
	vk.yscale2D = 1.0f;

	vk.clearAttachment = qtrue;
	vk.fboActive = qfalse;
	vk.offscreenRender = qfalse;
	vk.bloomActive = qfalse;
	vk.dglowActive = qfalse;
	vk.refractionActive = qfalse;
	vk.vboWorldActive = qfalse;
	vk.vboGhoul2Active = qfalse;
	vk.vboMdvActive = qfalse;
	vk.hw_fog = 0;
	vk.renderPassIndex = RENDER_PASS_MAIN;
	vk.ctmu = 0;

	vk.samplers.filter_min = GL_LINEAR_MIPMAP_LINEAR;
	vk.samplers.filter_max = GL_LINEAR;

	gles3_create_geometry_buffers( VERTEX_BUFFER_SIZE, INDEX_BUFFER_SIZE );
	gles3_init_programs();

	// Port stage 2 (gameport/docs/fbo_module.md): render into an offscreen
	// FBO sized to match the window instead of straight into the default
	// framebuffer, then blit it to the screen with a quad. Sized to
	// renderWidth/renderHeight (== vidWidth/vidHeight, scale 1.0 here) - the
	// same "logical" render target size gles3_get_viewport_rect already uses
	// (gles3_frame.cpp), so nothing else has to change to keep the viewport
	// correct.
	gles3_fbo_init_program();
	// gles3_fbo_resize(), not a bare gles3_fbo_create(): this is the call
	// vid_restart lands on after a real window resize (shared/sdl/
	// sdl_input.cpp SDL_WINDOWEVENT_SIZE_CHANGED -> "vid_restart" ->
	// CL_InitRef -> RE_Init -> vk_create_window -> here), with
	// vk.renderWidth/Height already updated to the new size above.
	gles3_fbo_resize( vk.renderWidth, vk.renderHeight );

	vk.pipelines_count = 0;
	vk.pipelines_world_base = 0;

	// default GL state
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glDisable( GL_BLEND );
	glDisable( GL_SCISSOR_TEST );
	glDisable( GL_POLYGON_OFFSET_FILL );
	glDisable( GL_STENCIL_TEST );
	glDepthMask( GL_TRUE );
	glDepthFunc( GL_LEQUAL );
	glFrontFace( GL_CCW );
	glViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	glScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClearDepthf( 1.0f );

	vk.active = qtrue;

	vk_render_splash();
}

void vk_create_window( void )
{
	if (glConfig.vidWidth == 0)
	{
		windowDesc_t windowDesc = { GRAPHICS_API_OPENGL };

		windowDesc.gl.profile = GLPROFILE_ES;
		windowDesc.gl.majorVersion = 3;
		windowDesc.gl.minorVersion = 0;

		// Mesa DRI3 (host verification, GLX ES contexts): glXSwapBuffers
		// throttles on present-completion events (xcb_wait_for_special_event).
		// On X servers whose present path degenerates (observed here: the
		// loading screen and the first 3D frame after map load each stalled
		// seconds-to-forever in SDL_GL_SwapWindow, wchan=do_poll), the game
		// loop freezes while the GPU is idle. vblank_mode=0 makes Mesa
		// present without vsync completion waits. Must be set before the
		// GL library/driver is initialized, i.e. before WIN_Init creates
		// the window and the first context. Harmless on non-Mesa stacks
		// (EGL/Wayland on the device ignore it).
		if (getenv( "vblank_mode" ) == NULL) {
			setenv( "vblank_mode", "0", 0 );
		}

		glConfig.deviceSupportsGamma = qfalse;
		Com_Memset(&glConfig, 0, sizeof(glConfig));

		window = ri.WIN_Init(&windowDesc, &glConfig);

		if (r_ignorehwgamma->integer)
			glConfig.deviceSupportsGamma = qfalse;

		gls.windowWidth = glConfig.vidWidth;
		gls.windowHeight = glConfig.vidHeight;

		gls.captureWidth = glConfig.vidWidth;
		gls.captureHeight = glConfig.vidHeight;

		vk_initialize();

		gls.initTime = ri.Milliseconds();
	}
	else if ( !vk.active )
	{
		// might happen after REF_KEEP_WINDOW (SP dead window)
		vk_initialize();
		gls.initTime = ri.Milliseconds();
	}

	if ( !vk.active ) {
		ri.Error( ERR_FATAL, "Recursive error during GLES3 initialization" );
	}

	glState.glStateBits = GLS_DEPTHTEST_DISABLE | GLS_DEPTHMASK_TRUE;

	tr.inited = qtrue;
}

void vk_release_resources( void )
{
	// GL objects owned by the backend; textures are released separately via
	// vk_delete_textures() before this is called (RE_Shutdown order).
	gles3_destroy_programs();
	gles3_destroy_geometry_buffers();
	gles3_uniform_shutdown();
	gles3_fbo_destroy_program();
	gles3_fbo_destroy();

	vk.active = qfalse;
}

void vk_shutdown( void )
{
	vk_release_resources();

	ri.WIN_Shutdown();

	Com_Memset(&glConfig, 0, sizeof(glConfig));
}
