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

// Frame management, geometry streaming and the GL state cache for rd-gles3.
// Replaces rd-vulkan vk_frame.cpp / vk_cmd.cpp / vk_swapchain.cpp.
//
// G1 renders directly into the backbuffer (no FBO); render-pass functions
// are therefore mostly semantic no-ops that keep the tr_backend frame
// structure intact for the G4 post-processing milestone.

#include "tr_local.h"
#include "gles3_local.h"

// (Matrix16Identity/Matrix16Copy/myGlMultMatrix are provided by tr_main.cpp)

void gles3_get_viewport_rect( int *x, int *y, int *w, int *h )
{
	if (backEnd.projection2D)
	{
		*x = 0;
		*y = 0;
		*w = vk.renderWidth;
		*h = vk.renderHeight;
	}
	else
	{
		// viewParms.viewportX/Y are already in GL window space (origin at the
		// bottom-left), which is why rd-vanilla passes them to glViewport
		// unchanged.  rd-vulkan flips y here only because Vulkan's framebuffer
		// origin is top-left - in GL that flip has to stay out.
		*x = backEnd.viewParms.viewportX * vk.renderScaleX;
		*y = backEnd.viewParms.viewportY * vk.renderScaleY;
		*w = (float)backEnd.viewParms.viewportWidth * vk.renderScaleX;
		*h = (float)backEnd.viewParms.viewportHeight * vk.renderScaleY;
	}
}

void gles3_get_scissor_rect( int *x, int *y, int *w, int *h )
{
	// port of rd-vulkan get_scissor_rect (vk_init.cpp:143), GL y convention
	if ( backEnd.viewParms.portalView != PV_NONE )
	{
		*x = backEnd.viewParms.scissorX;
		*y = backEnd.viewParms.scissorY;
		*w = backEnd.viewParms.scissorWidth;
		*h = backEnd.viewParms.scissorHeight;
		return;
	}

	gles3_get_viewport_rect( x, y, w, h );

	if ( *x < 0 ) *x = 0;
	if ( *y < 0 ) *y = 0;

	if ( *x + *w > glConfig.vidWidth )
		*w = glConfig.vidWidth - *x;
	if ( *y + *h > glConfig.vidHeight )
		*h = glConfig.vidHeight - *y;
}

void gles3_update_depth_range( Vk_Depth_Range depth_range )
{
	GLfloat depth_min, depth_max;
	int x, y, w, h;

	if ( vk.cmd->depth_range == depth_range )
		return;

	vk.cmd->depth_range = depth_range;

	switch ( depth_range ) {
		default:
		case DEPTH_RANGE_NORMAL:
			depth_min = 0.0f; depth_max = 1.0f;
			break;
		case DEPTH_RANGE_ZERO:
			depth_min = 0.0f; depth_max = 0.0f;
			break;
		case DEPTH_RANGE_ONE:
			depth_min = 1.0f; depth_max = 1.0f;
			break;
		case DEPTH_RANGE_WEAPON:
			depth_min = 0.0f; depth_max = 0.3f;
			break;
	}

	gles3_get_viewport_rect( &x, &y, &w, &h );
	glViewport( x, y, w, h );

	// rd-vulkan re-binds the scissor together with the viewport; mirror and
	// portal sub-views restrict rendering to the portal surface's screen
	// rect, without which the portal scene overwrites the whole framebuffer
	gles3_get_scissor_rect( &x, &y, &w, &h );
	glScissor( x, y, w, h );
	glEnable( GL_SCISSOR_TEST );
	glDepthRangef( depth_min, depth_max );
}

// ---------------------------------------------------------------------------
// state cache
// ---------------------------------------------------------------------------

typedef struct gles3_state_s {
	uint32_t		state_bits;		// GLS_* bits currently applied
	cullType_t		face_culling;
	qboolean		polygon_offset;
	qboolean		valid;

	// Stencil test and color write mask are part of the pipeline object in
	// Vulkan, so every vkCmdBindPipeline re-establishes them.  In GL they are
	// sticky global state: vk_bind_pipeline has to replay them, and the cache
	// must be dropped whenever the state may have changed behind our back
	// (frame start, render-pass switch, clears).
	Vk_Shadow_Phase	shadow_phase;
	cullType_t		shadow_cull;
	qboolean		shadow_valid;
	qboolean		colormask_off;
	qboolean		colormask_valid;
} gles3_state_t;

static gles3_state_t glStateCache;

static byte *s_index_stage; // host shadow for the streaming index buffer

void gles3_state_cache_invalidate( void )
{
	glStateCache.valid = qfalse;
	glStateCache.shadow_valid = qfalse;
	glStateCache.colormask_valid = qfalse;
	gles3_attribs_invalidate();
}

// Stencil state of the Q3 shadow-volume pipelines, mirrored from
// vk_pipelines.cpp:1268-1302.  SHADOW_EDGES counts front/back faces of the
// volume into the stencil buffer, SHADOW_FS_QUAD darkens only the pixels the
// count marked.  Note the increment/decrement side follows def->face_culling,
// i.e. the *unmirrored* cull mode, exactly like the Vulkan pipeline does.
void gles3_set_shadow_phase( Vk_Shadow_Phase phase, cullType_t face_culling )
{
	if ( glStateCache.shadow_valid && glStateCache.shadow_phase == phase &&
		( phase != SHADOW_EDGES || glStateCache.shadow_cull == face_culling ) )
	{
		return;
	}

	switch ( phase ) {
		case SHADOW_EDGES:
			glEnable( GL_STENCIL_TEST );
			glStencilMask( 255 );
			glStencilFunc( GL_ALWAYS, 0, 255 );
			glStencilOp( GL_KEEP, GL_KEEP,
				( face_culling == CT_FRONT_SIDED ) ? GL_INCR : GL_DECR );
			break;

		case SHADOW_FS_QUAD:
			glEnable( GL_STENCIL_TEST );
			glStencilMask( 255 );
			glStencilFunc( GL_NOTEQUAL, 0, 255 );
			glStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
			break;

		default:
		case SHADOW_DISABLED:
			glDisable( GL_STENCIL_TEST );
			// leave the write mask open so glClear(GL_STENCIL_BUFFER_BIT) works
			glStencilMask( 255 );
			break;
	}

	glStateCache.shadow_phase = phase;
	glStateCache.shadow_cull = face_culling;
	glStateCache.shadow_valid = qtrue;
}

// Vulkan counterpart: attachment_blend_state.colorWriteMask
// (vk_pipelines.cpp:1306) - shadow volumes and the TYPE_DOT occlusion probe
// write depth/stencil only.
void gles3_set_colormask( qboolean write_color )
{
	if ( glStateCache.colormask_valid && glStateCache.colormask_off == (qboolean)!write_color )
		return;

	if ( write_color )
		glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	else
		glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );

	glStateCache.colormask_off = (qboolean)!write_color;
	glStateCache.colormask_valid = qtrue;
}

void gles3_set_state( uint32_t state_bits, cullType_t face_culling, qboolean polygon_offset )
{
	uint32_t diff;

	if ( glStateCache.valid &&
		glStateCache.state_bits == state_bits &&
		glStateCache.face_culling == face_culling &&
		glStateCache.polygon_offset == polygon_offset )
	{
		return;
	}

	diff = glStateCache.valid ? (glStateCache.state_bits ^ state_bits) : ~0u;

	// depth
	if ( diff & GLS_DEPTHTEST_DISABLE ) {
		if ( state_bits & GLS_DEPTHTEST_DISABLE )
			glDisable( GL_DEPTH_TEST );
		else
			glEnable( GL_DEPTH_TEST );
	}

	// rd-vulkan (USE_REVERSED_DEPTH off) uses VK_COMPARE_OP_LESS_OR_EQUAL
	// with depth clear 1.0; the MVP z-remap in get_mvp_transform maps the
	// projection back to that same [0(near)..1(far)] window-depth convention.
	if ( diff & GLS_DEPTHFUNC_EQUAL )
		glDepthFunc( (state_bits & GLS_DEPTHFUNC_EQUAL) ? GL_EQUAL : GL_LEQUAL );

	if ( diff & GLS_DEPTHMASK_TRUE )
		glDepthMask( (state_bits & GLS_DEPTHMASK_TRUE) ? GL_TRUE : GL_FALSE );

	// blend
	if ( diff & GLS_BLEND_BITS ) {
		uint32_t src = state_bits & GLS_SRCBLEND_BITS;
		uint32_t dst = state_bits & GLS_DSTBLEND_BITS;
		GLenum sfactor = GL_ONE, dfactor = GL_ZERO;

		if ( src == 0 && dst == 0 ) {
			glDisable( GL_BLEND );
		} else {
			switch ( src ) {
				case GLS_SRCBLEND_ZERO:					sfactor = GL_ZERO; break;
				case GLS_SRCBLEND_ONE:					sfactor = GL_ONE; break;
				case GLS_SRCBLEND_DST_COLOR:			sfactor = GL_DST_COLOR; break;
				case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:	sfactor = GL_ONE_MINUS_DST_COLOR; break;
				case GLS_SRCBLEND_SRC_ALPHA:			sfactor = GL_SRC_ALPHA; break;
				case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	sfactor = GL_ONE_MINUS_SRC_ALPHA; break;
				case GLS_SRCBLEND_DST_ALPHA:			sfactor = GL_DST_ALPHA; break;
				case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:	sfactor = GL_ONE_MINUS_DST_ALPHA; break;
				case GLS_SRCBLEND_ALPHA_SATURATE:		sfactor = GL_SRC_ALPHA_SATURATE; break;
				default:								sfactor = GL_ONE; break;
			}
			switch ( dst ) {
				case GLS_DSTBLEND_ZERO:					dfactor = GL_ZERO; break;
				case GLS_DSTBLEND_ONE:					dfactor = GL_ONE; break;
				case GLS_DSTBLEND_SRC_COLOR:			dfactor = GL_SRC_COLOR; break;
				case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:	dfactor = GL_ONE_MINUS_SRC_COLOR; break;
				case GLS_DSTBLEND_SRC_ALPHA:			dfactor = GL_SRC_ALPHA; break;
				case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	dfactor = GL_ONE_MINUS_SRC_ALPHA; break;
				case GLS_DSTBLEND_DST_ALPHA:			dfactor = GL_DST_ALPHA; break;
				case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:	dfactor = GL_ONE_MINUS_DST_ALPHA; break;
				default:								dfactor = GL_ZERO; break;
			}
			glBlendFunc( sfactor, dfactor );
			glEnable( GL_BLEND );
		}
	}

	glStateCache.state_bits = state_bits;

	// Face culling.  rd-vulkan declares VK_FRONT_FACE_CLOCKWISE ("Q3 defaults
	// to clockwise vertex order", vk_pipelines.cpp:1218) and culls BACK for
	// CT_FRONT_SIDED; with the unflipped GL projection that is literally
	// glFrontFace(GL_CW) + glCullFace(GL_BACK), and equals rd-vanilla's
	// CCW + cull FRONT.  The mirror flag swaps the cull side upstream in
	// vk_bind_pipeline, exactly like rd-vulkan's rasterizer does.
	if ( !glStateCache.valid || glStateCache.face_culling != face_culling ) {
		switch ( face_culling ) {
			case CT_FRONT_SIDED:
				glCullFace( GL_BACK );
				glFrontFace( GL_CW );
				glEnable( GL_CULL_FACE );
				break;
			case CT_BACK_SIDED:
				glCullFace( GL_FRONT );
				glFrontFace( GL_CW );
				glEnable( GL_CULL_FACE );
				break;
			case CT_TWO_SIDED:
			default:
				glDisable( GL_CULL_FACE );
				break;
		}
		glStateCache.face_culling = face_culling;
	}

	// polygon offset
	if ( !glStateCache.valid || glStateCache.polygon_offset != polygon_offset ) {
		if ( polygon_offset ) {
			glEnable( GL_POLYGON_OFFSET_FILL );
			glPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );
		} else {
			glDisable( GL_POLYGON_OFFSET_FILL );
		}
		glStateCache.polygon_offset = polygon_offset;
	}

	glStateCache.valid = qtrue;
}

// ---------------------------------------------------------------------------
// geometry streaming
// ---------------------------------------------------------------------------

void gles3_geometry_buffer_reset( void )
{
	vk.vertex_buffer_offset = 0;
	vk.index_buffer_offset = 0;
	vk.geometry_buffer_size_new = 0;
	vk.index_buffer_size_new = 0;
}

qboolean gles3_geometry_buffer_overflow( void )
{
	return ( vk.geometry_buffer_size_new || vk.index_buffer_size_new ) ? qtrue : qfalse;
}

// Reserve `size` bytes in a host-side region; returns host pointer and the
// device offset.  On overflow the frame's draws are skipped (same contract
// as the Vulkan backend's geometry_buffer_size_new).
byte *gles3_geometry_buffer_map( uint32_t *offset, uint32_t size )
{
	byte *ptr;
	uint32_t aligned;

	aligned = PAD( vk.vertex_buffer_offset, 16 );

	if ( aligned + size > vk.geometry_buffer_size ) {
		// same contract as rd-vulkan vk_bind_attr: record the size the frame
		// would have needed, skip the rest of it, grow the buffer at end of
		// frame.  Without the skip the stale offsets of the previous draw
		// would be reused and the scene would flicker with garbage geometry.
		vk.geometry_buffer_size_new = log2pad( aligned + size, 1 );
		return NULL;
	}

	*offset = aligned;
	vk.vertex_buffer_offset = aligned + size;

	ptr = vk.geometry_buffer + aligned;
	Com_Memset( ptr, 0, size );
	return ptr;
}

byte *gles3_index_buffer_map( uint32_t *offset, uint32_t size )
{
	if ( s_index_stage == NULL ) {
		s_index_stage = (byte*)malloc( vk.index_buffer_size );
		if ( s_index_stage == NULL ) {
			ri.Error( ERR_FATAL, "gles3: can't allocate index stage buffer" );
		}
	}

	if ( vk.index_buffer_offset + size > vk.index_buffer_size ) {
		vk.index_buffer_size_new = log2pad( vk.index_buffer_offset + size, 1 );
		return NULL;
	}

	*offset = vk.index_buffer_offset;
	vk.index_buffer_offset += size;

	return s_index_stage + *offset;
}

void gles3_index_stage_destroy( void )
{
	if ( s_index_stage ) {
		free( s_index_stage );
		s_index_stage = NULL;
	}
}

void gles3_flush_geometry( void )
{
	// uploads happen per-draw in vk_bind_geometry/vk_tess_index via
	// glBufferSubData; nothing to do here
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

void vk_begin_frame( void )
{
	gles3_geometry_buffer_reset();
	gles3_uniform_reset();
	gles3_state_cache_invalidate();

	vk.cmd->num_indexes = 0;
	vk.cmd->index_offset = 0;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;

	glBindBuffer( GL_ARRAY_BUFFER, vk.vertex_buffer );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, vk.index_buffer );

	// Orphan both streaming buffers' storage once per frame (re-specify with
	// the same size, NULL data) instead of letting glBufferSubData/
	// glMapBufferRange reuse the previous frame's storage starting at offset
	// 0. Without this, the very first upload of a frame aliases bytes a
	// draw call from the *previous* frame may still be reading on a tiled
	// mobile GPU (Mali), forcing the driver to insert a GPU sync point
	// before the write is allowed to proceed - a frame-to-frame stall on
	// top of the per-draw-call cost that gles3_upload_buffer_range below
	// addresses. This mirrors reference_port quake3e renderergles3
	// gles3_map_geometry_buffer(), which orphans for the same reason.
	glBufferData( GL_ARRAY_BUFFER, vk.geometry_buffer_size, NULL, GL_STREAM_DRAW );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, vk.index_buffer_size, NULL, GL_STREAM_DRAW );

	vk.renderPassIndex = RENDER_PASS_MAIN;
}

void vk_end_frame( void )
{
	// rd-vulkan grows the geometry buffer at end of frame after an overflow
	// (vk_frame.cpp:1415 -> vk_resize_geometry_buffer); one frame is lost.
	if ( vk.geometry_buffer_size_new || vk.index_buffer_size_new )
		gles3_resize_geometry_buffers();

	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
}

void vk_present_frame( void )
{
	ri.WIN_Present( &window );
	// the presented frame completed on the CPU side (glFinish semantics of
	// the swap): backend is idle, screenshot/video readback may run
	vk.cmd->waitForFence = qtrue;
}

void vk_wait_idle( void )
{
	glFinish();
}

void vk_queue_wait_idle( void )
{
	glFinish();
}

void vk_end_render_pass( void )
{
	// G1: direct backbuffer rendering, nothing to end
}

void vk_begin_main_render_pass( void )
{
	vk.renderPassIndex = RENDER_PASS_MAIN;
	// vkCmdBeginRenderPass resets the dynamic state in rd-vulkan
	// (vk_frame.cpp:970); GL keeps it, so force the viewport/scissor/depth
	// range to be re-issued on the next draw
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
}

void vk_begin_post_refraction_extract_render_pass( void )
{
	vk.renderPassIndex = RENDER_PASS_REFRACTION;
}

void vk_refraction_extract( void )
{
	// G4: copy color attachment into the refraction texture
}

void vk_clear_color_attachments( const vec4_t color )
{
	int x, y, w, h;

	gles3_get_viewport_rect( &x, &y, &w, &h );
	glScissor( x, y, w, h );
	glEnable( GL_SCISSOR_TEST );
	// glClear honours the color write mask, which the shadow-volume and
	// depth-fragment pipelines leave switched off
	gles3_set_colormask( qtrue );
	glClearColor( color[0], color[1], color[2], color[3] );
	glClear( GL_COLOR_BUFFER_BIT );
	// keep scissor enabled; draws re-set scissor == viewport per view
}

void vk_clear_depthstencil_attachments( qboolean clear_stencil )
{
	GLbitfield mask = GL_DEPTH_BUFFER_BIT;
	int x, y, w, h;

	// glClear(GL_DEPTH_BUFFER_BIT) is a no-op while depth writes are masked
	// (the 2D/HUD stage of the previous frame leaves glDepthMask(GL_FALSE)).
	glDepthMask( GL_TRUE );

	// Like rd-vulkan's scissor-bounded vkCmdClearAttachments: a portal /
	// mirror sub-view clears depth only within its scissor rect, the main
	// view's depth outside the portal region must survive. glClear respects
	// the scissor test, so set it to the current view rect explicitly.
	gles3_get_scissor_rect( &x, &y, &w, &h );
	glScissor( x, y, w, h );
	glEnable( GL_SCISSOR_TEST );
	// glClear honours the stencil write mask too, so make sure the shadow
	// state is re-applied (and the mask left open) before clearing
	gles3_state_cache_invalidate();
	glStencilMask( 255 );

	// rd-vulkan (non-reversed) clears depth to 1.0 = farthest
	glClearDepthf( 1.0f );

	if ( clear_stencil )
		mask |= GL_STENCIL_BUFFER_BIT;

	glClear( mask );
}

void vk_begin_dglow_extract_render_pass( void )
{
	vk.renderPassIndex = RENDER_PASS_DGLOW;
}

void vk_begin_dglow_blur_render_pass( uint32_t index )
{
	(void)index;
}

qboolean vk_begin_dglow_blur( void )
{
	vk.renderPassIndex = RENDER_PASS_MAIN;
	return qtrue;
}

qboolean vk_bloom( void )
{
	return qfalse;
}

void vk_update_post_process_pipelines( void )
{
}

void vk_release_world_vbo( void )
{
	vk.vboWorldActive = qfalse;
}

void vk_release_model_vbo( void )
{
	vk.vboGhoul2Active = qfalse;
	vk.vboMdvActive = qfalse;
}

void vk_read_pixels( byte *buffer, uint32_t width, uint32_t height )
{
	byte *tmp = (byte*)malloc( width * 4 );
	uint32_t y, x;

	if ( tmp == NULL )
		return;

	// Output contract matches rd-vulkan vk_read_pixels: tightly packed RGB,
	// rows bottom-up - which is exactly glReadPixels' own order, so rows are
	// copied straight through.  (The extra flip that used to live here only
	// compensated for the upside-down 3D projection.)
	for ( y = 0; y < height; y++ ) {
		byte *dst = buffer + y * width * 3;
		glReadPixels( 0, y, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, tmp );
		for ( x = 0; x < width; x++ ) {
			dst[x * 3 + 0] = tmp[x * 4 + 0];
			dst[x * 3 + 1] = tmp[x * 4 + 1];
			dst[x * 3 + 2] = tmp[x * 4 + 2];
		}
	}

	free( tmp );
}
