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

// rd-gles3 backend header (fork of rd-vulkan vk_local.h).
//
// The vk_* symbol names are kept on purpose: the tr_*/G2_* frontend layer is
// shared with rd-vulkan and calls this API directly (same convention as
// quake3e renderergles3 which kept Vk_Pipeline_Def / Vk_Depth_Range names).
// Internally everything is implemented on top of GLES3 (see gles3_api.h);
// Vulkan types survive only as opaque typedefs where tr_local.h embeds them
// (image_t, VBO_t, IBO_t).

#pragma once

// keep system GL headers out; we declare the ES3 subset we use ourselves
#ifndef __gl3_h_
#define __gl3_h_
#endif
#ifndef __gl2_h_
#define __gl2_h_
#endif
#ifndef __gl31_h_
#define __gl31_h_
#endif

#include "gles3_api.h"

#ifndef MAX
#define MAX(x,y) ((x)>(y)?(x):(y))
#endif

#ifndef MIN
#define MIN(x,y) ((x)<(y)?(x):(y))
#endif

#define	REFRACTION_EXTRACT_SCALE		2
#define VK_NUM_BLUR_PASSES				4

// opaque vulkan handles kept only for tr_local.h struct members
typedef uint32_t					VkBool32;
typedef uint64_t					VkDeviceSize;
typedef struct VkImage_T*			VkImage;
typedef struct VkImageView_T*		VkImageView;
typedef struct VkDescriptorSet_T*	VkDescriptorSet;
typedef struct VkBuffer_T*			VkBuffer;
typedef struct VkDeviceMemory_T*	VkDeviceMemory;
typedef GLuint						VkSampler;

#define VK_NULL_HANDLE				0

typedef enum {
	VK_FORMAT_BC3_UNORM_BLOCK			= 0x1,
	VK_FORMAT_B8G8R8A8_UNORM			= 0x2,
	VK_FORMAT_R8G8B8A8_UNORM			= 0x3,
	VK_FORMAT_R8G8B8_UNORM			= 0x4,
	VK_FORMAT_B4G4R4A4_UNORM_PACK16	= 0x5,
	VK_FORMAT_A1R5G5B5_UNORM_PACK16	= 0x6,
} VkFormatCompat;

typedef enum {
	VK_SAMPLER_ADDRESS_MODE_REPEAT			= 0,
	VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT	= 1,
	VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE	= 2,
	VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER	= 3
} VkSamplerAddressMode;

typedef enum {
	VK_INDEX_TYPE_UINT16 = 0,
	VK_INDEX_TYPE_UINT32 = 1,
} VkIndexType;

// geometry buffer layout (streaming, host shadow + glBufferSubData)
#define VERTEX_CHUNK_SIZE				( 768 * 1024 )

#define XYZ_SIZE						( 4 * VERTEX_CHUNK_SIZE )
#define COLOR_SIZE						( 3 * VERTEX_CHUNK_SIZE )
#define ST0_SIZE						( 1 * VERTEX_CHUNK_SIZE )
#define ST1_SIZE						( 1 * VERTEX_CHUNK_SIZE )
#define ST2_SIZE						( 1 * VERTEX_CHUNK_SIZE )
#define NNN_SIZE						( 2 * VERTEX_CHUNK_SIZE )

#define XYZ_OFFSET						0
#define COLOR_OFFSET					( XYZ_OFFSET + XYZ_SIZE )
#define ST0_OFFSET						( COLOR_OFFSET + COLOR_SIZE )
#define ST1_OFFSET						( ST0_OFFSET + ST0_SIZE )
#define ST2_OFFSET						( ST1_OFFSET + ST1_SIZE )
#define NNN_OFFSET						( ST2_OFFSET + ST2_SIZE )

#define VERTEX_BUFFER_SIZE				( NNN_OFFSET + NNN_SIZE )

#define INDEX_BUFFER_SIZE				( 1024 * 1024 )

#define TESS_XYZ						( 1 )
#define TESS_RGBA0 						( 2 )
#define TESS_RGBA1 						( 4 )
#define TESS_RGBA2 						( 8 )
#define TESS_ST0   						( 16 )
#define TESS_ST1   						( 32 )
#define TESS_ST2   						( 64 )
#define TESS_NNN   						( 128 )
#define TESS_VPOS  						( 256 )	// uniform with eyePos
#define TESS_ENV   						( 512 )	// mark shader stage with environment mapping

// extra math
#define DotProduct4( a , b )			((a)[0]*(b)[0] + (a)[1]*(b)[1] + (a)[2]*(b)[2] + (a)[3]*(b)[3])
#define VectorScale4( a , b , c )		((c)[0]=(a)[0]*(b),(c)[1]=(a)[1]*(b),(c)[2]=(a)[2]*(b),(c)[3]=(a)[3]*(b))
#define Vector4Set( v, x, y, z, w )		((v)[0]=(x),(v)[1]=(y),(v)[2]=(z),v[3]=(w))
#define Vector4Copy( a, b )				((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2],(b)[3]=(a)[3])
#define LERP( a, b, w )					((a)*(1.0f-(w))+(b)*(w))
#define LUMA( r, g, b )					(0.2126f*(r)+0.7152f*(g)+0.0722f*(b))
#define EPSILON 1e-6f
#ifndef SGN
#define SGN( x )						(((x) >= 0) ? !!(x) : -1)
#endif

typedef float mat4_t[16];
typedef float mat3x4_t[12];
typedef unsigned int uvec4_t[4];

#define BUFFER_OFFSET(i) ((char *)NULL + (i))

void Matrix16Identity( mat4_t out );
void Matrix16Copy( const mat4_t in, mat4_t out );
void myGlMultMatrix( const float *a, const float *b, float *out );

typedef union floatint_u
{
	int32_t		i;
	uint32_t	u;
	float		f;
	byte		b[4];
} floatint_t;

// state bits, identical values to rd-vulkan so tr_shader emits the same defs
#define GLS_SRCBLEND_ZERO						0x00000001
#define GLS_SRCBLEND_ONE						0x00000002
#define GLS_SRCBLEND_DST_COLOR					0x00000003
#define GLS_SRCBLEND_ONE_MINUS_DST_COLOR		0x00000004
#define GLS_SRCBLEND_SRC_ALPHA					0x00000005
#define GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA		0x00000006
#define GLS_SRCBLEND_DST_ALPHA					0x00000007
#define GLS_SRCBLEND_ONE_MINUS_DST_ALPHA		0x00000008
#define GLS_SRCBLEND_ALPHA_SATURATE				0x00000009
#define	GLS_SRCBLEND_BITS		    			0x0000000f

#define GLS_DSTBLEND_ZERO						0x00000010
#define GLS_DSTBLEND_ONE						0x00000020
#define GLS_DSTBLEND_SRC_COLOR					0x00000030
#define GLS_DSTBLEND_ONE_MINUS_SRC_COLOR		0x00000040
#define GLS_DSTBLEND_SRC_ALPHA					0x00000050
#define GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA		0x00000060
#define GLS_DSTBLEND_DST_ALPHA					0x00000070
#define GLS_DSTBLEND_ONE_MINUS_DST_ALPHA		0x00000080
#define	GLS_DSTBLEND_BITS					    0x000000f0

#define GLS_BLEND_BITS							( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS )

#define GLS_DEPTHMASK_TRUE						0x00000100

#define GLS_POLYMODE_LINE						0x00001000

#define GLS_DEPTHTEST_DISABLE					0x00010000
#define GLS_DEPTHFUNC_EQUAL						0x00020000

#define GLS_ATEST_GT_0							0x10000000
#define GLS_ATEST_LT_80							0x20000000
#define GLS_ATEST_GE_80							0x40000000
#define GLS_ATEST_GE_C0							0x80000000
#define	GLS_ATEST_BITS					    	0xF0000000

#define GLS_DEFAULT								GLS_DEPTHMASK_TRUE

#ifndef GL_REPEAT
#define GL_REPEAT				0x2901
#endif
#ifndef GL_CLAMP
#define GL_CLAMP				0x2900
#endif

typedef enum {
	TYPE_COLOR_BLACK,
	TYPE_COLOR_WHITE,
	TYPE_COLOR_GREEN,
	TYPE_COLOR_RED,
	TYPE_FOG_ONLY,
	TYPE_DOT,
	TYPE_REFRACTION,

	TYPE_SINGLE_TEXTURE_LIGHTING,
	TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR,

	TYPE_SINGLE_TEXTURE_DF,

	TYPE_GENERIC_BEGIN, // start of non-env/env shader pairs
	TYPE_SINGLE_TEXTURE = TYPE_GENERIC_BEGIN,
	TYPE_SINGLE_TEXTURE_ENV,

	TYPE_SINGLE_TEXTURE_IDENTITY,
	TYPE_SINGLE_TEXTURE_IDENTITY_ENV,

	TYPE_SINGLE_TEXTURE_FIXED_COLOR,
	TYPE_SINGLE_TEXTURE_FIXED_COLOR_ENV,

	TYPE_MULTI_BEGIN, // start of multi-textured stages
	TYPE_MULTI_TEXTURE_ADD2_IDENTITY = TYPE_MULTI_BEGIN,
	TYPE_MULTI_TEXTURE_ADD2_IDENTITY_ENV,
	TYPE_MULTI_TEXTURE_MUL2_IDENTITY,
	TYPE_MULTI_TEXTURE_MUL2_IDENTITY_ENV,

	TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR,
	TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR_ENV,
	TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR,
	TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR_ENV,

	TYPE_MULTI_TEXTURE_MUL2,
	TYPE_MULTI_TEXTURE_MUL2_ENV,
	TYPE_MULTI_TEXTURE_ADD2_1_1,
	TYPE_MULTI_TEXTURE_ADD2_1_1_ENV,
	TYPE_MULTI_TEXTURE_ADD2,
	TYPE_MULTI_TEXTURE_ADD2_ENV,

	TYPE_MULTI_TEXTURE_MUL3,
	TYPE_MULTI_TEXTURE_MUL3_ENV,
	TYPE_MULTI_TEXTURE_ADD3_1_1,
	TYPE_MULTI_TEXTURE_ADD3_1_1_ENV,
	TYPE_MULTI_TEXTURE_ADD3,
	TYPE_MULTI_TEXTURE_ADD3_ENV,

	TYPE_BLEND2_ADD,
	TYPE_BLEND2_ADD_ENV,
	TYPE_BLEND2_MUL,
	TYPE_BLEND2_MUL_ENV,
	TYPE_BLEND2_ALPHA,
	TYPE_BLEND2_ALPHA_ENV,
	TYPE_BLEND2_ONE_MINUS_ALPHA,
	TYPE_BLEND2_ONE_MINUS_ALPHA_ENV,
	TYPE_BLEND2_MIX_ALPHA,
	TYPE_BLEND2_MIX_ALPHA_ENV,
	TYPE_BLEND2_MIX_ONE_MINUS_ALPHA,
	TYPE_BLEND2_MIX_ONE_MINUS_ALPHA_ENV,

	TYPE_BLEND2_DST_COLOR_SRC_ALPHA,
	TYPE_BLEND2_DST_COLOR_SRC_ALPHA_ENV,

	TYPE_BLEND3_ADD,
	TYPE_BLEND3_ADD_ENV,
	TYPE_BLEND3_MUL,
	TYPE_BLEND3_MUL_ENV,
	TYPE_BLEND3_ALPHA,
	TYPE_BLEND3_ALPHA_ENV,
	TYPE_BLEND3_ONE_MINUS_ALPHA,
	TYPE_BLEND3_ONE_MINUS_ALPHA_ENV,
	TYPE_BLEND3_MIX_ALPHA,
	TYPE_BLEND3_MIX_ALPHA_ENV,
	TYPE_BLEND3_MIX_ONE_MINUS_ALPHA,
	TYPE_BLEND3_MIX_ONE_MINUS_ALPHA_ENV,

	TYPE_BLEND3_DST_COLOR_SRC_ALPHA,
	TYPE_BLEND3_DST_COLOR_SRC_ALPHA_ENV,

	TYPE_GENERIC_END = TYPE_BLEND3_MIX_ONE_MINUS_ALPHA_ENV

} Vk_Shader_Type;

// used with cg_shadows == 2
typedef enum {
	SHADOW_DISABLED,
	SHADOW_EDGES,
	SHADOW_FS_QUAD,
} Vk_Shadow_Phase;

typedef enum {
	TRIANGLE_LIST = 0,
	TRIANGLE_STRIP,
	LINE_LIST,
	POINT_LIST
} Vk_Primitive_Topology;

typedef enum {
	DEPTH_RANGE_NORMAL, // [0..1]
	DEPTH_RANGE_ZERO, // [0..0]
	DEPTH_RANGE_ONE, // [1..1]
	DEPTH_RANGE_WEAPON, // [0..0.3]
	DEPTH_RANGE_COUNT
} Vk_Depth_Range;

typedef enum {
	RENDER_PASS_MAIN = 0,
	RENDER_PASS_SCREENMAP,
	RENDER_PASS_POST_BLEND,
	RENDER_PASS_DGLOW,
	RENDER_PASS_REFRACTION,
	RENDER_PASS_COUNT
} renderPass_t;

typedef struct {
	uint32_t				state_bits; // GLS_XXX flags
	cullType_t				face_culling;// cullType_t

	qboolean				polygon_offset;
	qboolean				mirror;
	Vk_Shader_Type			shader_type;
	Vk_Shadow_Phase			shadow_phase;
	Vk_Primitive_Topology	primitives;
	uint32_t				surface_sprite_flags;

	int line_width;
	int fog_stage; // off, fog-in / fog-out
	int abs_light;
	int allow_discard;
	int acff; // none, rgb, rgba, alpha
	struct {
		byte rgb;
		byte alpha;
	} color;
} Vk_Pipeline_Def;

// Uniform locations for one linked program, resolved once at link time
// (gles3_shaders.cpp) instead of being queried from the driver on every
// draw call (see gles3_apply_uniforms in gles3_geometry.cpp).
typedef struct gles3_uniform_locs_s {
	GLint	u_MVP;
	GLint	u_ModelMatrix;
	GLint	u_EyePos;
	GLint	u_LightPos;
	GLint	u_LightColor;
	GLint	u_LightVector;
	GLint	u_FogDistanceVector;
	GLint	u_FogDepthVector;
	GLint	u_FogEyeT;
	GLint	u_FogColor;
	GLint	u_Texture0;
	GLint	u_Texture1;
	GLint	u_Texture2;
	GLint	u_TextureFog;
	GLint	u_FixedColor;
	GLint	u_AlphaTest;
} gles3_uniform_locs_t;

typedef struct VK_Pipeline {
	Vk_Pipeline_Def def;
	GLuint			program;
	const gles3_uniform_locs_t *locs;
} VK_Pipeline_t;

typedef struct vktcMod_s {
	vec4_t	matrix;
	vec4_t	offTurb;
} vktcMod_t;

typedef struct vktcGen_s {
	vec3_t	vector0;
	int32_t	pad0;
	vec3_t	vector1;
	int32_t	type;
} vktcGen_t;

// this structure must be in sync with shader uniforms!
typedef struct vkUniform_s {
	// light/env/material parameters:
	vec4_t eyePos;
	vec4_t lightPos;
	vec4_t lightColor; // rgb + 1/(r*r)
	vec4_t lightVector;

	// fog parameters:
	union {
		struct {
			vec4_t fogDistanceVector;
			vec4_t fogDepthVector;
			vec4_t fogEyeT;
			vec4_t fogColor;
		} fog;

		struct {
			vktcMod_t tcMod;
			vktcGen_t tcGen;
		} refraction;
	};

	mat4_t	modelMatrix;
} vkUniform_t;

typedef struct vkUniformCamera_s {
	vec4_t viewOrigin;
} vkUniformCamera_t;

typedef struct vkUniformEntity_s {
	vec4_t ambientLight;
	vec4_t directedLight;
	vec4_t lightOrigin;
	vec4_t localViewOrigin;
	mat4_t modelMatrix;
} vkUniformEntity_t;

typedef struct vkUniformFogEntry_s {
	vec4_t	plane;
	vec4_t	color;
	float	depthToOpaque;
	int		hasPlane;
	vec2_t	pad0;
} vkUniformFogEntry_t;

typedef struct vkUniformFog_s {
	int			num_fogs;
	vec3_t		pad0;
	vkUniformFogEntry_t fogs[16];
} vkUniformFog_t;

// ghoul2 bone matrices block (tr_ghoul2.cpp uses it unconditionally)
typedef struct vkUniformBones_s {
	mat3x4_t boneMatrices[72];
} vkUniformBones_t;

typedef struct {
	VkSamplerAddressMode address_mode; // clamp/repeat texture addressing mode

	int gl_mag_filter; // GL_XXX mag filter
	int gl_min_filter; // GL_XXX min filter

	qboolean max_lod_1_0; // fixed 1.0 lod
	qboolean noAnisotropy;
} Vk_Sampler_Def;

struct Image_Upload_Data  {
	byte *buffer;
	int buffer_size;
	int mip_levels;
	int base_level_width;
	int base_level_height;
};

extern unsigned char s_intensitytable[256];
extern unsigned char s_gammatable[256];
extern unsigned char s_gammatable_linear[256];

// Vk_World contains backend resources/state requested by the game code.
// It is reinitialized on a map change.
typedef struct {
	// MVP
	float modelview_transform[16]  QALIGN(16);
} Vk_World;

typedef struct vk_tess_s {
	Vk_Depth_Range		depth_range;

	uint32_t			num_indexes; // value from most recent vk_bind_index() call
	uint32_t			index_offset; // device offset from the same call

	uint32_t			camera_ubo_offset;
	uint32_t			entity_ubo_offset[REFENTITYNUM_WORLD + 1];
	uint32_t			bones_ubo_offset;
	uint32_t			fogs_ubo_offset;

	qboolean			waitForFence;
} vk_tess_t;

#define MAX_VK_PIPELINES				( 1024 + 256 )

// Vk_Instance contains backend resources that persist the renderer lifetime,
// initialized/deinitialized by vk_initialize/vk_shutdown.
typedef struct {
	// to prevent changes to rd-common, move these here
	char			renderer_string[MAX_STRING_CHARS];
	char			vendor_string[MAX_STRING_CHARS];
	char			version_string[MAX_STRING_CHARS];
	char			device_extensions_string[MAX_STRING_CHARS];

	qboolean		active;
	qboolean		clearAttachment;	// backend supports attachment clear (always true on GL)
	qboolean		fboActive;
	qboolean		blitEnabled;

	qboolean		vboWorldActive;
	qboolean		vboGhoul2Active;
	qboolean		vboMdvActive;

	float			maxAnisotropy;
	float			maxLod;

	qboolean		bloomActive;
	qboolean		dglowActive;
	qboolean		refractionActive;

	qboolean		offscreenRender;
	qboolean		windowAdjusted;
	int				blitX0;
	int				blitY0;
	int				blitFilter;

	uint32_t		hw_fog;	// "hardware" fog mode: r_drawfog 2

	uint32_t		screenMapWidth;
	uint32_t		screenMapHeight;

	int				ctmu;	// current texture index

	uint32_t		renderWidth;
	uint32_t		renderHeight;

	float			renderScaleX;
	float			renderScaleY;
	float			yscale2D;
	float			xscale2D;

	renderPass_t	renderPassIndex;

	uint32_t		maxBoundDescriptorSets;

	// uniform emulation: offsets returned by vk_append_uniform()
	uint32_t		uniform_item_size;
	uint32_t		uniform_camera_item_size;
	uint32_t		uniform_entity_item_size;
	uint32_t		uniform_fogs_item_size;
	uint32_t		uniform_bones_item_size;

	// geometry streaming
	GLuint			vertex_buffer;		// streaming VBO (region layout above)
	GLuint			index_buffer;		// streaming index buffer
	byte			*geometry_buffer;	// host shadow, malloc'd
	uint32_t		geometry_buffer_size;
	uint32_t		geometry_buffer_size_new;	// on overflow: required size, frame's draws are skipped
	uint32_t		index_buffer_size;
	uint32_t		index_buffer_size_new;		// same contract for the index stream
	uint32_t		vertex_buffer_offset;
	uint32_t		index_buffer_offset;

	vk_tess_t		tess[1], *cmd;
	int				cmd_index;

	// pipeline cache
	struct  {
		uint32_t skybox_pipeline;
		uint32_t worldeffect_pipeline[2];

		// dim 0: 0 - front side, 1 - back size
		// dim 1: 0 - normal view, 1 - mirror view
		uint32_t shadow_volume_pipelines[2][2];
		uint32_t shadow_finish_pipeline;

		// dim 0 is based on fogPass_t: 0 - corresponds to FP_EQUAL, 1 - corresponds to FP_LE.
		// dim 1 is directly a cullType_t enum value.
		// dim 2 is a polygon offset value (0 - off, 1 - on).
		uint32_t fog_pipelines[3][2][3][2];

#ifdef USE_PMLIGHT
		// cullType[3], polygonOffset[2], fogStage[2], absLight[2]
		uint32_t dlight_pipelines_x[3][2][2][2];
		uint32_t dlight1_pipelines_x[3][2][2][2];
#endif

		// debug-visualization pipelines
		uint32_t tris_debug_pipeline;
		uint32_t tris_mirror_debug_pipeline;
		uint32_t tris_debug_green_pipeline;
		uint32_t tris_mirror_debug_green_pipeline;
		uint32_t tris_debug_red_pipeline;
		uint32_t tris_mirror_debug_red_pipeline;

		uint32_t normals_debug_pipeline;
		uint32_t surface_debug_pipeline_solid;
		uint32_t surface_debug_pipeline_outline;
		uint32_t images_debug_pipeline;
		uint32_t surface_beam_pipeline;
		uint32_t surface_axis_pipeline;
		uint32_t dot_pipeline;
	} std_pipeline;

	VK_Pipeline_t	pipelines[MAX_VK_PIPELINES];
	uint32_t		pipelines_count;
	uint32_t		pipelines_world_base;

	// current uniform block (filled by the stage iterator, uploaded at draw)
	vkUniform_t		uniform;

	// global texture filter (glCompat enum values, see tr_local.h)
	struct samplers_s {
		int filter_min;
		int filter_max;
	} samplers;

	struct {
		GLuint		image;
	} capture;

} Vk_Instance;

extern Vk_Instance	vk;				// shouldn't be cleared during ref re-init
extern Vk_World		vk_world;		// this data is cleared during ref re-init

// init/window
void		vk_set_clearcolor( void );
void		vk_create_window( void );
void		vk_initialize( void );
void		vk_shutdown( void );
qboolean	R_CanMinimize( void );

// pipeline
void		vk_create_pipelines(void);
void		vk_update_post_process_pipelines( void );

// frame
void		vk_begin_frame( void );
void		vk_end_frame( void );
void		vk_present_frame( void );
void		vk_wait_idle( void );
void		vk_queue_wait_idle( void );
void		vk_release_resources( void );
void		vk_read_pixels( byte *buffer, uint32_t width, uint32_t height );

// vbo (world/model VBO arrive in G5/G3; names kept for tr_init)
void		vk_release_world_vbo( void );
void		vk_release_model_vbo( void );

// image
// (vk_bind/vk_upload_image_data/vk_generate_image_upload_data/vk_create_image/
//  RE_UploadCinematic are declared in tr_local.h after image_t; DrawTris/
//  DrawNormals live there too. Only the texture-mode entry points are
//  vk_local.h-specific.)
void		vk_texture_mode( const char *string, const qboolean init );
void		vk_delete_textures( void );

// shade geometry
void		vk_set_2d( void );
void		vk_set_depthrange( const Vk_Depth_Range depthRange );
void		vk_update_mvp( const float *m );

void		vk_select_texture( const int index );
uint32_t	vk_tess_index( uint32_t numIndexes, const void *src );
void		vk_bind_index( void );
void		vk_bind_index_ext( const int numIndexes, const uint32_t *indexes );
void		vk_bind_pipeline( uint32_t pipeline );
void		vk_draw_geometry( Vk_Depth_Range depth_range, qboolean indexed );
void		vk_draw_dot( uint32_t storage_offset );
void		vk_bind_geometry( uint32_t flags );
void		vk_bind_lighting( int stage, int bundle );
uint32_t	vk_find_pipeline_ext( uint32_t base, const Vk_Pipeline_Def *def, qboolean use );
void		vk_get_pipeline_def( uint32_t pipeline, Vk_Pipeline_Def *def );
uint32_t	vk_append_uniform( const void *uniform, size_t size, uint32_t min_offset );

// render passes (G1: direct backbuffer rendering, mostly no-ops)
void		vk_end_render_pass( void );
void		vk_begin_main_render_pass( void );
void		vk_clear_color_attachments( const vec4_t color );
void		vk_clear_depthstencil_attachments( qboolean clear_stencil );
void		vk_begin_post_refraction_extract_render_pass( void );
void		vk_refraction_extract( void );
qboolean	vk_bloom( void );
void		vk_begin_dglow_extract_render_pass( void );
void		vk_begin_dglow_blur_render_pass( uint32_t index );
qboolean	vk_begin_dglow_blur( void );

// post-processing / lighting (stubs until G4)
#ifdef USE_PMLIGHT
void		vk_lighting_pass( void );
#endif

// info
const char	*vk_shadertype_string( Vk_Shader_Type code );
void		vk_info_f( void );
void		GfxInfo_f( void );

// CPU image processing (vk_image_process.cpp)
void		R_LightScaleTexture( byte *in, int inwidth, int inheight, qboolean only_gamma );
void		ResampleTexture( unsigned *in, int inwidth, int inheight, unsigned *out, int outwidth, int outheight );
void		R_BlendOverTexture( unsigned char *data, const uint32_t pixelCount, const uint32_t l );
void		R_MipMap( byte *out, byte *in, int width, int height );
void		R_MipMap2( unsigned* const out, unsigned* const in, int inWidth, int inHeight );

// debug
void		vk_debug( const char *msg, ... );
void		R_DebugGraphics( void );

// gles3 shader/program cache (gles3_shaders.cpp)
void		gles3_init_programs( void );
void		gles3_destroy_programs( void );
GLuint		gles3_get_program( const Vk_Pipeline_Def *def, const gles3_uniform_locs_t **out_locs );

// gles3 geometry streaming helpers (gles3_frame.cpp)
void		gles3_geometry_buffer_reset( void );
qboolean	gles3_geometry_buffer_overflow( void );
byte		*gles3_geometry_buffer_map( uint32_t *offset, uint32_t size );
byte		*gles3_index_buffer_map( uint32_t *offset, uint32_t size );

#define VK_CHECK( function_call ) function_call
