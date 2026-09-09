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

// Image pool / upload / filtering for rd-gles3, ported from rd-vulkan
// vk_image.cpp.  Textures are plain GL_TEXTURE_2D objects; per-texture
// sampler state is applied at creation (GL tex parameters), mip data is
// uploaded level-by-level with glTexSubImage2D (no staging buffers).

#include "tr_local.h"
#include "gles3_local.h"
#include "../rd-common/tr_common.h"

#define	DEFAULT_SIZE	16
#define FILE_HASH_SIZE	1024

#define IMAGE_POOL_INITIAL_CAPACITY 1024
#define IMAGE_UPLOAD_SCRATCH_MIN    (64 * 1024 * 1024)
#define IMAGE_RESAMPLE_SCRATCH_MIN  (32 * 1024 * 1024)

// image_t.handle is a VkImage (opaque pointer typedef) in the shared
// tr_local.h; we store the GL texture name in it via uintptr_t round-trip.
// (G3_IMG_H is shared via gles3_local.h; G3_IMG_SET stays local.)
#define G3_IMG_SET(img, id)	((img)->handle = (VkImage)(uintptr_t)(id))

static image_t *hashTable[FILE_HASH_SIZE];

// defined here (were vk_instance.cpp in rd-vulkan); used by tr_image.cpp and
// vk_image_process.cpp
unsigned char s_intensitytable[256];
unsigned char s_gammatable[256];
unsigned char s_gammatable_linear[256];

#define	DLIGHT_SIZE		16
#define	FOG_S			256
#define	FOG_T			32

static int Hex( char c )
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + c - 'A';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + c - 'a';
	}

	return -1;
}

typedef struct imageScratchBuffer_s {
	byte *buffer;
	size_t size;
} imageScratchBuffer_t;

typedef struct imageScratch_s {
	imageScratchBuffer_t upload;
	imageScratchBuffer_t resample;
	imageScratchBuffer_t mip;
	imageScratchBuffer_t convert;
} imageScratch_t;

static imageScratch_t s_imageScratch;

static byte* R_ImageScratchAlloc( imageScratchBuffer_t *scratch, size_t size )
{
	if ( scratch->buffer == NULL ) {
		scratch->buffer = (byte*)malloc( size );
		scratch->size = size;
	} else if ( scratch->size < size ) {
		free( scratch->buffer );
		scratch->buffer = (byte*)malloc( size );
		scratch->size = size;
	}

	if ( scratch->buffer == NULL ) {
		ri.Error( ERR_FATAL, "R_ImageScratchAlloc: can't allocate %u bytes", (unsigned)size );
	}

	return scratch->buffer;
}

void R_InitImageScratch( void )
{
	Com_Memset( &s_imageScratch, 0, sizeof(s_imageScratch) );
}

static void R_FreeScratch( imageScratchBuffer_t* scratch )
{
	if ( scratch->buffer != NULL ) {
		free( scratch->buffer );
	}
	Com_Memset( scratch, 0, sizeof( *scratch ) );
}

void R_DestroyImageScratch( void )
{
	R_FreeScratch( &s_imageScratch.upload );
	R_FreeScratch( &s_imageScratch.resample );
	R_FreeScratch( &s_imageScratch.mip );
	R_FreeScratch( &s_imageScratch.convert );

	Com_Memset( &s_imageScratch, 0, sizeof(s_imageScratch) );
}

void R_InitImagesPool( void )
{
	tr.images.items = (image_t**)malloc( IMAGE_POOL_INITIAL_CAPACITY * sizeof(image_t*) );
	if ( tr.images.items == NULL ) {
		ri.Error( ERR_FATAL, "R_InitImagesPool: can't allocate image pool" );
	}
	tr.images.capacity = IMAGE_POOL_INITIAL_CAPACITY;
	tr.images.count = 0;
}

void R_DestroyImagesPool( void )
{
	if ( tr.images.items != NULL ) {
		free( tr.images.items );
		tr.images.items = NULL;
	}
	tr.images.capacity = 0;
	tr.images.count = 0;
}

static void R_AddImageToPool(image_t *image)
{
	if ( tr.images.items == NULL ) {
		return;
	}

	if ( tr.images.count >= tr.images.capacity ) {
		image_t **new_pool = (image_t**)realloc( tr.images.items, tr.images.capacity * 2 * sizeof(image_t*) );
		if ( new_pool == NULL ) {
			ri.Error( ERR_FATAL, "R_AddImageToPool: can't grow image pool" );
		}
		tr.images.items = new_pool;
		tr.images.capacity *= 2;
	}

	tr.images.items[tr.images.count] = image;
	tr.images.count++;
}

static int generateHashValue( const char *fname )
{
    uint32_t i = 0;
    int	hash = 0;

    while (fname[i] != '\0') {
        char letter = tolower(fname[i]);
        if (letter == '.') break;		// don't include extension
        if (letter == '\\') letter = '/';	// damn path names
        hash += (long)(letter) * (i + 119);
        i++;
    }
    hash &= (FILE_HASH_SIZE - 1);

    return hash;
}

/*
================
GetTextureMode
================
*/
static const textureMode_t modes[] = {
	{"GL_NEAREST", GL_NEAREST, GL_NEAREST},
	{"GL_LINEAR", GL_LINEAR, GL_LINEAR},
	{"GL_NEAREST_MIPMAP_NEAREST", GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST},
	{"GL_LINEAR_MIPMAP_NEAREST", GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR},
	{"GL_NEAREST_MIPMAP_LINEAR", GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST},
	{"GL_LINEAR_MIPMAP_LINEAR", GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR}
};

textureMode_t *GetTextureMode( const char *name ) {
	int i;

	if ( name == NULL ) {
		return NULL;
	}

	for ( i = 0; i < (int)ARRAY_LEN( modes ); i++ ) {
		if ( !Q_stricmp( name, modes[i].name ) ) {
			return (textureMode_t*)&modes[i];
		}
	}

	return NULL;
}

int gl_filter_min = GL_LINEAR_MIPMAP_LINEAR;
int gl_filter_max = GL_LINEAR;

// map tr_local.h glCompat filter enum to real GL values
static GLenum gles3_filter( int compat, qboolean mipmap )
{
	if ( !mipmap ) {
		return ( compat == GL_NEAREST || compat == GL_NEAREST_MIPMAP_NEAREST || compat == GL_NEAREST_MIPMAP_LINEAR ) ? G3_NEAREST : G3_LINEAR;
	}

	switch ( compat ) {
		case GL_NEAREST: return G3_NEAREST;
		case GL_LINEAR: return G3_LINEAR;
		case GL_NEAREST_MIPMAP_NEAREST: return G3_NEAREST_MIPMAP_NEAREST;
		case GL_LINEAR_MIPMAP_NEAREST: return G3_LINEAR_MIPMAP_NEAREST;
		case GL_NEAREST_MIPMAP_LINEAR: return G3_NEAREST_MIPMAP_LINEAR;
		default: return G3_LINEAR_MIPMAP_LINEAR;
	}
}

void vk_texture_mode( const char *string, const qboolean init ) {
	const textureMode_t *mode;
	image_t	*img;
	int		i;

	mode = GetTextureMode( string );

	if ( mode == NULL ) {
		ri.Printf( PRINT_ALL, "bad texture filter name '%s'\n", string );
		return;
	}

	gl_filter_min = mode->minimize;
	gl_filter_max = mode->maximize;

	if( init ){
		r_textureMode->modified = qfalse; // no need to do this a second time in tr_cmds
		vk.samplers.filter_min = gl_filter_min;
		vk.samplers.filter_max = gl_filter_max;
		return;
	}

	if ( gl_filter_min == vk.samplers.filter_min && gl_filter_max == vk.samplers.filter_max ) {
		return;
	}

	vk_wait_idle();

	vk.samplers.filter_min = gl_filter_min;
	vk.samplers.filter_max = gl_filter_max;

	for ( i = 0; i < tr.images.count; i++ ) {
		img = tr.images.items[i];
		glBindTexture( GL_TEXTURE_2D, G3_IMG_H(img->handle) );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gles3_filter( gl_filter_max, qfalse ) );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gles3_filter( gl_filter_min, (img->flags & IMGFLAG_MIPMAP) ? qtrue : qfalse ) );
	}
	glBindTexture( GL_TEXTURE_2D, 0 );
}

/*
================
vk_bind
================
*/
void vk_bind( image_t *image )
{
	if ( !image ) {
		ri.Error( ERR_DROP, "vk_bind: NULL image" );
	}

	glActiveTexture( GL_TEXTURE0 + vk.ctmu );
	glBindTexture( GL_TEXTURE_2D, G3_IMG_H(image->handle) );
}

void vk_update_descriptor_set( image_t *image )
{
	// GL: sampler state lives on the texture object itself
	(void)image;
}

void vk_create_image( image_t *image, int width, int height, int mip_levels ) {
	qboolean mipmap = mip_levels > 1 ? qtrue : qfalse;

	if ( image->handle ) {
		GLuint h = G3_IMG_H(image->handle);
		glDeleteTextures( 1, &h );
		image->handle = 0;
	}

	{
		GLuint h;
		glGenTextures( 1, &h );
		G3_IMG_SET(image, h);
		glBindTexture( GL_TEXTURE_2D, h );
	}

	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gles3_filter( gl_filter_max, qfalse ) );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gles3_filter( gl_filter_min, mipmap ) );

	if ( image->wrapClampMode == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE ) {
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	}
	else if ( image->wrapClampMode == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT ) {
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT );
	}
	else {
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	}

	// allocate storage for all mip levels
	{
		int w = width;
		int h = height;
		int level;

		for ( level = 0; level < mip_levels; level++ ) {
			glTexImage2D( GL_TEXTURE_2D, level, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
			if ( w == 1 && h == 1 )
				break;
			if ( w > 1 ) w >>= 1;
			if ( h > 1 ) h >>= 1;
		}
	}

	glBindTexture( GL_TEXTURE_2D, 0 );
}

void vk_upload_image_data( image_t *image, int x, int y, int width,
	int height, int mipmaps, byte *pixels, int size, qboolean update )
{
	byte *buf = pixels;
	int mip_level_size;
	int num_regions = 0;

	(void)update;

	glBindTexture( GL_TEXTURE_2D, G3_IMG_H(image->handle) );

	while ( qtrue ) {
		glTexSubImage2D( GL_TEXTURE_2D, num_regions, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buf );

		mip_level_size = width * height * 4;
		buf += mip_level_size;

		num_regions++;

		if ( num_regions >= mipmaps || (width == 1 && height == 1) || num_regions >= 16 )
			break;

		x >>= 1;
		y >>= 1;

		width >>= 1;
		if (width < 1) width = 1;

		height >>= 1;
		if (height < 1) height = 1;
	}

	glBindTexture( GL_TEXTURE_2D, 0 );

	(void)size;
}

static qboolean RawImage_HasAlpha( const byte *scan, const int numPixels )
{
	int i;

	if ( !scan )
		return qtrue;

	for ( i = 0; i < numPixels; i++ ) {
		if ( scan[i*4 + 3] != 255 )
			return qtrue;
	}

	return qfalse;
}

static int R_CalcUploadSize( int width, int height, qboolean mipmap, int bytesPerPixel )
{
	int total = 0;

	while (1)
	{
		total += width * height * bytesPerPixel;

		if (!mipmap || (width == 1 && height == 1))
			break;

		width >>= 1;
		height >>= 1;

		if (width < 1) width = 1;
		if (height < 1) height = 1;
	}

	return total;
}

void vk_generate_image_upload_data(image_t* image, byte* data, Image_Upload_Data* upload_data)
{
	qboolean mipmap = (image->flags & IMGFLAG_MIPMAP) ? qtrue : qfalse;
	qboolean picmip = (image->flags & IMGFLAG_PICMIP) ? qtrue : qfalse;

	byte* resampled_buffer = NULL;
	byte* mip_buffer = NULL;

	int scaled_width, scaled_height;
	int width = image->width;
	int height = image->height;

	int mip_level_size;
	int miplevel;

	int bytesPerPixel = 4;
	int write_offset = 0;

	if (image->flags & IMGFLAG_NOSCALE) {
		scaled_width = width;
		scaled_height = height;
	}
	else {
		for (scaled_width = 1; scaled_width < width; scaled_width <<= 1);
		for (scaled_height = 1; scaled_height < height; scaled_height <<= 1);

		if (r_roundImagesDown->integer && scaled_width > width)
			scaled_width >>= 1;

		if (r_roundImagesDown->integer && scaled_height > height)
			scaled_height >>= 1;
	}

	if (picmip && (tr.mapLoading || r_nomip->integer == 0)) {
		scaled_width >>= r_picmip->integer;
		scaled_height >>= r_picmip->integer;
	}

	if (scaled_width < 1)
		scaled_width = 1;

	if (scaled_height < 1)
		scaled_height = 1;

	if (!(image->flags & IMGFLAG_LIGHTMAP)) {
		while (scaled_width > glConfig.maxTextureSize || scaled_height > glConfig.maxTextureSize) {
			scaled_width >>= 1;
			scaled_height >>= 1;
		}
	}

	upload_data->base_level_width = scaled_width;
	upload_data->base_level_height = scaled_height;

	image->internalFormat = VK_FORMAT_R8G8B8A8_UNORM;

	upload_data->buffer_size = R_CalcUploadSize( scaled_width, scaled_height, mipmap, bytesPerPixel );

	upload_data->buffer = R_ImageScratchAlloc( &s_imageScratch.upload, MAX(upload_data->buffer_size, IMAGE_UPLOAD_SCRATCH_MIN) );

	if (data == NULL) {
		Com_Memset(upload_data->buffer, 0, upload_data->buffer_size);
		upload_data->mip_levels = 1;
		return;
	}

	if ((scaled_width != width || scaled_height != height) && data) {
		resampled_buffer = R_ImageScratchAlloc( &s_imageScratch.resample, MAX(scaled_width * scaled_height * 4, IMAGE_RESAMPLE_SCRATCH_MIN) );

		ResampleTexture((unsigned*)data, width, height, (unsigned*)resampled_buffer, scaled_width, scaled_height);

		mip_buffer = resampled_buffer;
	}
	else {
		mip_buffer = R_ImageScratchAlloc( &s_imageScratch.mip, scaled_width * scaled_height * 4 );

		Com_Memcpy(mip_buffer, data, scaled_width * scaled_height * 4);
	}

	width = scaled_width;
	height = scaled_height;

	if (image->flags & IMGFLAG_COLORSHIFT) {
		byte* p = mip_buffer;
		int i, n = width * height;

		for (i = 0; i < n; i++, p += 4)
			R_ColorShiftLightingBytes(p, p, qfalse);
	}

	while (width > scaled_width || height > scaled_height) {
		R_MipMap(mip_buffer, mip_buffer, width, height);

		width >>= 1;
		if (width < 1)
			width = 1;

		height >>= 1;
		if (height < 1)
			height = 1;
	}

	if (!(image->flags & IMGFLAG_NOLIGHTSCALE)) {
		R_LightScaleTexture(mip_buffer, scaled_width, scaled_height, mipmap ? qfalse : qtrue);
	}

	mip_level_size = scaled_width * scaled_height * bytesPerPixel;

	Com_Memcpy(&upload_data->buffer[write_offset], mip_buffer, mip_level_size);

	write_offset += mip_level_size;

	miplevel = 0;

	if (mipmap) {
		while (scaled_width > 1 || scaled_height > 1) {
			R_MipMap(mip_buffer, mip_buffer, scaled_width, scaled_height);

			scaled_width >>= 1;
			if (scaled_width < 1)
				scaled_width = 1;

			scaled_height >>= 1;
			if (scaled_height < 1)
				scaled_height = 1;

			miplevel++;

			if (r_colorMipLevels->integer) {
				R_BlendOverTexture(mip_buffer, scaled_width * scaled_height, miplevel);
			}

			mip_level_size = scaled_width * scaled_height * bytesPerPixel;

			Com_Memcpy(&upload_data->buffer[write_offset], mip_buffer, mip_level_size);

			write_offset += mip_level_size;

			if (scaled_width == 1 && scaled_height == 1)
				break;
		}
	}

	upload_data->buffer_size = write_offset;
	upload_data->mip_levels = miplevel + 1;
}

void vk_upload_image( image_t *image, byte *pic ) {

	Image_Upload_Data upload_data;
	int w, h;

	vk_generate_image_upload_data( image, pic, &upload_data );

	w = upload_data.base_level_width;
	h = upload_data.base_level_height;

	image->uploadWidth = w;
	image->uploadHeight = h;

	vk_create_image( image, w, h, upload_data.mip_levels );
	vk_upload_image_data( image, 0, 0, w, h, upload_data.mip_levels, upload_data.buffer, upload_data.buffer_size, qfalse );
}

void vk_delete_textures( void ) {
	int i;

	vk_wait_idle();

	if ( tr.images.count == 0 ) {
		return;
	}

	for (i = 0; i < tr.images.count; i++) {
		image_t *img = tr.images.items[i];
		if ( img->handle ) {
			GLuint h = G3_IMG_H(img->handle);
			glDeleteTextures( 1, &h );
			img->handle = 0;
		}
	}

	R_DestroyImagesPool();

	Com_Memset(tr.scratchImage, 0, sizeof(tr.scratchImage));
	Com_Memset(glState.currenttextures, 0, sizeof(glState.currenttextures));
}

image_t *R_CreateImage( const char *name, byte *pic, int width, int height, imgFlags_t flags ){
    image_t				*image;
    int					namelen;
    long				hash;

    namelen = (int)strlen(name) + 1;
    if (namelen > MAX_QPATH) {
        ri.Error(ERR_DROP, "R_CreateImage: \"%s\" is too long", name);
    }

    image = (image_t*)Z_Malloc(sizeof(*image) + namelen, TAG_IMAGE_T);
	Com_Memset(image, 0, sizeof(*image) + namelen);

    image->imgName = (char*)(image + 1);
    strcpy(image->imgName, name);

    hash = generateHashValue(name);
    image->next = hashTable[hash];
    hashTable[hash] = image;

    R_AddImageToPool(image);

    image->flags = flags;
    image->width = width;
    image->height = height;

    if (namelen > 6 && Q_stristr(image->imgName, "maps/") == image->imgName && Q_stristr(image->imgName + 6, "/lm_") != NULL) {
        // external lightmap atlases stored in maps/<mapname>/lm_XXXX textures
		image->flags |= IMGFLAG_NO_COMPRESSION | IMGFLAG_NOSCALE;
    }

    if (flags & IMGFLAG_CLAMPTOBORDER)
        image->wrapClampMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    else if (flags & IMGFLAG_CLAMPTOEDGE)
        image->wrapClampMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    else
        image->wrapClampMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;

	if(r_smartpicmip && r_smartpicmip->integer && Q_stricmpn(name, "textures/", 9)) {
		image->flags &= ~(IMGFLAG_PICMIP);
	}

	image->handle = VK_NULL_HANDLE;
	image->view = VK_NULL_HANDLE;
	image->descriptor_set = VK_NULL_HANDLE;

    vk_upload_image( image, pic );

    return image;
}

image_t *R_FindImageFile( const char *name, imgFlags_t flags ){
        image_t		*image;
        int			width, height;
        byte		*pic;
        int			hash;

		if (!name || ri.Cvar_VariableIntegerValue("dedicated"))	// stop ghoul2 horribleness as regards image loading from server
		{
			return NULL;
		}

        hash = generateHashValue(name);

        //
        // see if the image is already loaded
        for (image = hashTable[hash]; image; image = image->next) {
            if (!Q_stricmp(name, image->imgName)) {
                // the white image can be used with any set of parms, but other mismatches are errors
                if (strcmp(name, "*white")) {
                    if (image->flags != flags) {
                        ri.Printf(PRINT_DEVELOPER, "WARNING: reused image %s with mixed flags (%i vs %i)\n", name, image->flags, flags);
                    }
                }
                return image;
            }
        }

        //
        // load the pic from disk
        //
        R_LoadImage(name, &pic, &width, &height);
        if (pic == NULL) {
            return NULL;
        }

		// refuse to find any files not power of 2 dims...
		//
		if ((width & (width - 1)) || (height & (height - 1)))
		{
			ri.Printf(PRINT_ALL, "Refusing to load non-power-2-dims(%d,%d) pic \"%s\"...\n", width, height, name);
			return NULL;
		}

        if (tr.mapLoading && r_mapGreyScale->value > 0) {
            byte *img;
            int i;
            for (i = 0, img = pic; i < width * height; i++, img += 4) {
                if (r_mapGreyScale->integer) {
                    byte luma = LUMA(img[0], img[1], img[2]);
                    img[0] = luma;
                    img[1] = luma;
                    img[2] = luma;
                }
                else {
                    float luma = LUMA(img[0], img[1], img[2]);
                    img[0] = LERP(img[0], luma, r_mapGreyScale->value);
                    img[1] = LERP(img[1], luma, r_mapGreyScale->value);
                    img[2] = LERP(img[2], luma, r_mapGreyScale->value);
                }
            }
		}

        image = R_CreateImage(name, pic, width, height, flags);
        ri.Z_Free(pic);
        return image;
}

void RE_UploadCinematic( int cols, int rows, const byte *data, int client, qboolean dirty )
{
	image_t *image;

    if ( !tr.scratchImage[client] ) {
		tr.scratchImage[client] = R_CreateImage(va("*scratch%i", client), (byte*)data, cols, rows,
			IMGFLAG_CLAMPTOEDGE | IMGFLAG_RGB | IMGFLAG_NOSCALE | IMGFLAG_NO_COMPRESSION);
		return;
    }

    image = tr.scratchImage[client];

    // if the scratchImage isn't in the format we want, specify it as a new texture
    if ( cols != image->width || rows != image->height )  {
		image->width = image->uploadWidth = cols;
		image->height = image->uploadHeight = rows;

		vk_create_image( image, cols, rows, 1 );
		vk_upload_image_data( image, 0, 0, cols, rows, 1, (byte*)data, cols * rows * 4, qfalse );

    }
    else if (dirty)
    {
        vk_upload_image_data( image, 0, 0, cols, rows, 1, (byte*)data, cols * rows * 4, qtrue );
    }
}

/*
================
R_BuildDefaultImage

Create solid color texture from following input formats (hex):
#rgb
#rrggbb
================
*/
static qboolean R_BuildDefaultImage( const char *format ) {
	byte data[DEFAULT_SIZE][DEFAULT_SIZE][4];
	byte color[4];
	int i, len, hex[6];
	int x, y;

	if (*format++ != '#') {
		return qfalse;
	}

	len = (int)strlen(format);
	if (len <= 0 || len > 6) {
		return qfalse;
	}

	for (i = 0; i < len; i++) {
		hex[i] = Hex(format[i]);
		if (hex[i] == -1) {
			return qfalse;
		}
	}

	switch (len) {
	case 3: // #rgb
		color[0] = hex[0] << 4 | hex[0];
		color[1] = hex[1] << 4 | hex[1];
		color[2] = hex[2] << 4 | hex[2];
		color[3] = 255;
		break;
	case 6: // #rrggbb
		color[0] = hex[0] << 4 | hex[1];
		color[1] = hex[2] << 4 | hex[3];
		color[2] = hex[4] << 4 | hex[5];
		color[3] = 255;
		break;
	default: // unsupported format
		return qfalse;
	}

	for (y = 0; y < DEFAULT_SIZE; y++) {
		for (x = 0; x < DEFAULT_SIZE; x++) {
			data[x][y][0] = color[0];
			data[x][y][1] = color[1];
			data[x][y][2] = color[2];
			data[x][y][3] = color[3];
		}
	}

	tr.defaultImage = R_CreateImage("*default", (byte *)data, DEFAULT_SIZE, DEFAULT_SIZE, IMGFLAG_MIPMAP);

	return qtrue;
}

static void R_CreateDefaultImage( void )
{
	uint32_t x;
	byte data[DEFAULT_SIZE][DEFAULT_SIZE][4];

	if (r_defaultImage->string[0])
	{
		// build from format
		if (R_BuildDefaultImage(r_defaultImage->string))
			return;

		// load from external file
		tr.defaultImage = R_FindImageFile(r_defaultImage->string, IMGFLAG_MIPMAP | IMGFLAG_PICMIP);
		if (tr.defaultImage)
			return;
	}

	// the default image will be a box, to allow you to see the mapping coordinates
	Com_Memset(data, 32, sizeof(data));

	for (x = 0; x < DEFAULT_SIZE; x++)
	{
		data[0][x][0] =
		data[0][x][1] =
		data[0][x][2] =
		data[0][x][3] = 255;

		data[x][0][0] =
		data[x][0][1] =
		data[x][0][2] =
		data[x][0][3] = 255;

		data[DEFAULT_SIZE - 1][x][0] =
		data[DEFAULT_SIZE - 1][x][1] =
		data[DEFAULT_SIZE - 1][x][2] =
		data[DEFAULT_SIZE - 1][x][3] = 255;

		data[x][DEFAULT_SIZE - 1][0] =
		data[x][DEFAULT_SIZE - 1][1] =
		data[x][DEFAULT_SIZE - 1][2] =
		data[x][DEFAULT_SIZE - 1][3] = 255;
	}

	tr.defaultImage = R_CreateImage("*default", (byte*)data, DEFAULT_SIZE, DEFAULT_SIZE, IMGFLAG_MIPMAP);
}

static void R_CreateDlightImage( void )
{
    int		width, height;
    byte	*pic;

    R_LoadImage("gfx/2d/dlight", &pic, &width, &height);
    if (pic)
    {
        tr.dlightImage = R_CreateImage("*dlight", pic, width, height, IMGFLAG_CLAMPTOEDGE);
        Z_Free(pic);
    }
    else
    {	// if we dont get a successful load
        int		x, y;
        byte	data[DLIGHT_SIZE][DLIGHT_SIZE][4];
        int		b;

        // make a centered inverse-square falloff blob for dynamic lighting
        for (x = 0; x < DLIGHT_SIZE; x++) {
            for (y = 0; y < DLIGHT_SIZE; y++) {
                float	d;

                d = (DLIGHT_SIZE / 2 - 0.5f - x) * (DLIGHT_SIZE / 2 - 0.5f - x) +
                    (DLIGHT_SIZE / 2 - 0.5f - y) * (DLIGHT_SIZE / 2 - 0.5f - y);
                b = 4000 / d;
                if (b > 255) {
                    b = 255;
                }
                else if (b < 75) {
                    b = 0;
                }
                data[y][x][0] =
                    data[y][x][1] =
                    data[y][x][2] = b;
                data[y][x][3] = 255;
            }
        }
        tr.dlightImage = R_CreateImage("*dlight", (byte*)data, DLIGHT_SIZE, DLIGHT_SIZE, IMGFLAG_CLAMPTOEDGE);
    }
}

static void R_CreateFogImage( void )
{
    int		x, y;
    byte	*data;
    float	d;

    data = (unsigned char*)Hunk_AllocateTempMemory(FOG_S * FOG_T * 4);

    // S is distance, T is depth
    for (x = 0; x < FOG_S; x++) {
        for (y = 0; y < FOG_T; y++) {
            d = R_FogFactor((x + 0.5f) / FOG_S, (y + 0.5f) / FOG_T);

            data[(y * FOG_S + x) * 4 + 0] =
            data[(y * FOG_S + x) * 4 + 1] =
            data[(y * FOG_S + x) * 4 + 2] = 255;
            data[(y * FOG_S + x) * 4 + 3] = 255 * d;
        }
    }

    tr.fogImage = R_CreateImage("*fog", data, FOG_S, FOG_T, IMGFLAG_CLAMPTOEDGE);
    Hunk_FreeTempMemory(data);
}

/*
==================
R_CreateBuiltinImages
==================
*/
static void R_CreateBuiltinImages( void ) {
	int		x, y;
	byte	data[DEFAULT_SIZE][DEFAULT_SIZE][4];

	R_CreateDefaultImage();

	// we use a solid white image instead of disabling texturing
	Com_Memset(data, 255, sizeof(data));
	tr.whiteImage = R_CreateImage("*white", (byte*)data, 8, 8, IMGFLAG_NONE);

	Com_Memset(data, 0, sizeof(data));
	tr.blackImage = R_CreateImage("*black", (byte*)data, 8, 8, IMGFLAG_NONE);

	// with overbright bits active, we need an image which is some fraction of full color,
	// for default lightmaps, etc
	for ( x = 0; x < DEFAULT_SIZE; x++ ) {
		for ( y = 0; y < DEFAULT_SIZE; y++ ) {
			data[y][x][0] =
			data[y][x][1] =
			data[y][x][2] = tr.identityLightByte;
			data[y][x][3] = 255;
		}
	}
	tr.identityLightImage = R_CreateImage("*identityLight", (byte*)data, 8, 8, IMGFLAG_NONE);

	R_CreateDlightImage();
	R_CreateFogImage();
}

void R_InitImages( void ) {
	int i;

	// initialize linear gamma table before setting color mappings for the first time
	for ( i = 0; i < 256; i++ )
		s_gammatable_linear[i] = (unsigned char)i;

	Com_Memset(hashTable, 0, sizeof(hashTable));
	R_InitImagesPool();
	R_InitImageScratch();

	// build brightness translation tables
	R_SetColorMappings();

	R_CreateBuiltinImages();

	vk_texture_mode( r_textureMode->string, qtrue );
}
