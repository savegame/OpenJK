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

// Minimal GLES3 (ES 3.0) API surface for the rd-gles3 renderer.
//
// The system <GLES3/gl3.h> is intentionally NOT included: tr_local.h defines
// its own GL_* compatibility names (glCompat enum with GL_NEAREST = 105 etc.)
// which collide with the real GL preprocessor constants.  All entry points
// are linked against libGLESv2 directly (the engine creates the EGL context
// via WIN_Init); no function-pointer indirection is needed.
//
// Constant VALUES are the real ES 3.0 values; names that would clash with
// tr_local.h's compat names carry the G3_ prefix.

#pragma once

typedef unsigned int	GLenum;
typedef unsigned char	GLboolean;
typedef unsigned int	GLbitfield;
typedef void			GLvoid;
typedef signed char		GLbyte;
typedef short			GLshort;
typedef int				GLint;
typedef int				GLsizei;
typedef unsigned char	GLubyte;
typedef unsigned short	GLushort;
typedef unsigned int	GLuint;
typedef float			GLfloat;
typedef float			GLclampf;
typedef char			GLchar;
typedef signed long		GLintptr;
typedef signed long		GLsizeiptr;
typedef long long		GLint64;
typedef unsigned long long GLuint64;

// filter names colliding with tr_local.h glCompat enum
#define G3_NEAREST						0x2600
#define G3_LINEAR						0x2601
#define G3_NEAREST_MIPMAP_NEAREST		0x2700
#define G3_LINEAR_MIPMAP_NEAREST		0x2701
#define G3_NEAREST_MIPMAP_LINEAR		0x2702
#define G3_LINEAR_MIPMAP_LINEAR			0x2703

// env/transfer names colliding with tr_local.h glCompat enum
#define G3_ADD							0x0104
#define G3_MODULATE						0x2100
#define G3_DECAL						0x2101

// polygon primitives (tr_local.h mirrors the same values)
#ifndef GL_POINTS
#define GL_POINTS						0x0000
#define GL_LINES						0x0001
#define GL_LINE_LOOP					0x0002
#define GL_LINE_STRIP					0x0003
#define GL_TRIANGLES					0x0004
#define GL_TRIANGLE_STRIP				0x0005
#define GL_TRIANGLE_FAN					0x0006
#endif

// buffer objects
#define GL_ARRAY_BUFFER					0x8892
#define GL_ELEMENT_ARRAY_BUFFER			0x8893
#define GL_STATIC_DRAW					0x88E4
#define GL_DYNAMIC_DRAW					0x88E8
#define GL_STREAM_DRAW					0x88E0
#define GL_WRITE_ONLY_OES				0x88B9
#define GL_BUFFER_SIZE					0x8764
#define GL_MAP_READ_BIT					0x0001
#define GL_MAP_WRITE_BIT				0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT		0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT	0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT		0x0010
#define GL_MAP_UNSYNCHRONIZED_BIT		0x0020

// texture targets / params
#define GL_TEXTURE_2D					0x0DE1
#define GL_TEXTURE_WRAP_S				0x2802
#define GL_TEXTURE_WRAP_T				0x2803
#define GL_TEXTURE_MAG_FILTER			0x2800
#define GL_TEXTURE_MIN_FILTER			0x2801
#define GL_TEXTURE0						0x84C0
#define GL_TEXTURE1						0x84C1
#define GL_TEXTURE2						0x84C2
#define GL_TEXTURE3						0x84C3
#define GL_TEXTURE4						0x84C4
#define GL_TEXTURE5						0x84C5
#define GL_TEXTURE6						0x84C6
#define GL_TEXTURE7						0x84C7
#define GL_REPEAT						0x2901
#define GL_CLAMP_TO_EDGE				0x812F
#define GL_MIRRORED_REPEAT				0x8370

#define GL_FALSE						0
#define GL_TRUE							1

// pixel formats/types
#define GL_UNSIGNED_BYTE				0x1401
#define GL_UNSIGNED_SHORT				0x1403
#define GL_UNSIGNED_INT					0x1405
#define GL_BYTE							0x1400
#define GL_SHORT						0x1402
#define GL_INT							0x1404
#define GL_FLOAT						0x1406
#define GL_RED							0x1903
#define GL_RG							0x8227
#define GL_RGB							0x1907
#define GL_RGBA							0x1908
#define GL_LUMINANCE					0x1909
#define GL_LUMINANCE_ALPHA				0x190A
#define GL_ALPHA						0x1906
#define GL_RGB565						0x8D62
#define GL_RGBA4						0x8056
#define GL_RGB5_A1						0x8057
#define GL_RGBA8						0x8058
#define GL_RGB8							0x8051
#define GL_RGB10_A2						0x8059
#define GL_R8							0x8229
#define GL_RG8							0x822B
#define GL_RGBA32F						0x8814
#define GL_RGB32F						0x8815
#define GL_HALF_FLOAT					0x140B

// samplers
#define GL_SAMPLER_BINDING				0x8919

// clear bits / test enables
#define GL_DEPTH_BUFFER_BIT				0x00000100
#define GL_STENCIL_BUFFER_BIT			0x00000400
#define GL_COLOR_BUFFER_BIT				0x00004000
#define GL_DEPTH_TEST					0x0B71
#define GL_STENCIL_TEST					0x0B90
#define GL_CULL_FACE					0x0B44
#define GL_BLEND						0x0BE2
#define GL_SCISSOR_TEST					0x0C11
#define GL_POLYGON_OFFSET_FILL			0x8037
#define GL_DITHER						0x0BD0

// compare functions
#define GL_NEVER						0x0200
#define GL_LESS							0x0201
#define GL_EQUAL						0x0202
#define GL_LEQUAL						0x0203
#define GL_GREATER						0x0204
#define GL_NOTEQUAL						0x0205
#define GL_GEQUAL						0x0206
#define GL_ALWAYS						0x0207

// blend factors
#define GL_ZERO							0
#define GL_ONE							1
#define GL_SRC_COLOR					0x0300
#define GL_ONE_MINUS_SRC_COLOR			0x0301
#define GL_SRC_ALPHA					0x0302
#define GL_ONE_MINUS_SRC_ALPHA			0x0303
#define GL_DST_ALPHA					0x0304
#define GL_ONE_MINUS_DST_ALPHA			0x0305
#define GL_DST_COLOR					0x0306
#define GL_ONE_MINUS_DST_COLOR			0x0307
#define GL_SRC_ALPHA_SATURATE			0x0308
#define GL_CONSTANT_COLOR				0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR		0x8002

// blend equations
#define GL_FUNC_ADD						0x8006
#define GL_FUNC_SUBTRACT				0x800A
#define GL_FUNC_REVERSE_SUBTRACT		0x800B
#define GL_MIN							0x8007
#define GL_MAX							0x8008

// face winding / culling
#define GL_CW							0x0900
#define GL_CCW							0x0901
#define GL_FRONT						0x0404
#define GL_BACK							0x0405
#define GL_FRONT_AND_BACK				0x0408

// errors
#define GL_NO_ERROR						0
#define GL_INVALID_ENUM					0x0500
#define GL_INVALID_VALUE				0x0501
#define GL_INVALID_OPERATION			0x0502
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506
#define GL_OUT_OF_MEMORY				0x0505

// strings / implementation limits
#define GL_VENDOR						0x1F00
#define GL_RENDERER						0x1F01
#define GL_VERSION						0x1F02
#define GL_EXTENSIONS					0x1F03
#define GL_SHADING_LANGUAGE_VERSION		0x8B8C
#define GL_CURRENT_PROGRAM				0x8B8D
#define GL_MAX_TEXTURE_SIZE				0x0D33
#define GL_MAX_TEXTURE_IMAGE_UNITS		0x8872
#define GL_MAX_VERTEX_ATTRIBS			0x8869
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_NUM_EXTENSIONS				0x821D

// shader/program
#define GL_FRAGMENT_SHADER				0x8B30
#define GL_VERTEX_SHADER				0x8B31
#define GL_COMPILE_STATUS				0x8B81
#define GL_LINK_STATUS					0x8B82
#define GL_INFO_LOG_LENGTH				0x8B84
#define GL_ACTIVE_UNIFORMS				0x8B86
#define GL_ACTIVE_ATTRIBUTES			0x8B89

// framebuffers / renderbuffers
#define GL_FRAMEBUFFER					0x8D40
#define GL_RENDERBUFFER					0x8D41
#define GL_COLOR_ATTACHMENT0			0x8CE0
#define GL_DEPTH_ATTACHMENT				0x8D00
#define GL_STENCIL_ATTACHMENT			0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT		0x821A
#define GL_DEPTH_COMPONENT16			0x81A5
#define GL_DEPTH_COMPONENT24			0x81A6
#define GL_DEPTH24_STENCIL8				0x88F0
#define GL_FRAMEBUFFER_COMPLETE			0x8CD5
#define GL_READ_FRAMEBUFFER				0x8CA8
#define GL_DRAW_FRAMEBUFFER				0x8CA9

// vertex attrib normalize flag
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED 0x886A

#ifdef __cplusplus
extern "C" {
#endif

const GLubyte *glGetString( GLenum name );
const GLubyte *glGetStringi( GLenum name, GLuint index );
GLenum glGetError( void );
void glGetIntegerv( GLenum pname, GLint *data );
void glGetBooleanv( GLenum pname, GLboolean *data );
void glGetFloatv( GLenum pname, GLfloat *data );
GLboolean glIsEnabled( GLenum cap );

void glEnable( GLenum cap );
void glDisable( GLenum cap );
void glClear( GLbitfield mask );
void glClearColor( GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha );
void glClearDepthf( GLfloat d );
void glViewport( GLint x, GLint y, GLsizei width, GLsizei height );
void glScissor( GLint x, GLint y, GLsizei width, GLsizei height );
void glDepthMask( GLboolean flag );
void glDepthFunc( GLenum func );
void glDepthRangef( GLfloat n, GLfloat f );
void glBlendFunc( GLenum sfactor, GLenum dfactor );
void glBlendFuncSeparate( GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha );
void glBlendEquation( GLenum mode );
void glCullFace( GLenum mode );
void glFrontFace( GLenum mode );
void glPolygonOffset( GLfloat factor, GLfloat units );
void glLineWidth( GLfloat width );
void glStencilFunc( GLenum func, GLint ref, GLuint mask );
void glStencilOp( GLenum fail, GLenum zfail, GLenum zpass );
void glStencilMask( GLuint mask );
void glColorMask( GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha );

void glActiveTexture( GLenum texture );
void glBindTexture( GLenum target, GLuint texture );
void glGenTextures( GLsizei n, GLuint *textures );
void glDeleteTextures( GLsizei n, const GLuint *textures );
void glTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels );
void glTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels );
void glTexParameteri( GLenum target, GLenum pname, GLint param );
void glTexParameterf( GLenum target, GLenum pname, GLfloat param );
void glGenerateMipmap( GLenum target );
void glCopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height );

void glGenSamplers( GLsizei count, GLuint *samplers );
void glDeleteSamplers( GLsizei count, const GLuint *samplers );
void glBindSampler( GLuint unit, GLuint sampler );
void glSamplerParameteri( GLuint sampler, GLenum pname, GLint param );

void glGenBuffers( GLsizei n, GLuint *buffers );
void glDeleteBuffers( GLsizei n, const GLuint *buffers );
void glBindBuffer( GLenum target, GLuint buffer );
void glBufferData( GLenum target, GLsizeiptr size, const void *data, GLenum usage );
void glBufferSubData( GLenum target, GLintptr offset, GLsizeiptr size, const void *data );
void *glMapBufferRange( GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access );
GLboolean glUnmapBuffer( GLenum target );

void glEnableVertexAttribArray( GLuint index );
void glDisableVertexAttribArray( GLuint index );
void glVertexAttribPointer( GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer );
void glVertexAttribDivisor( GLuint index, GLuint divisor );
void glDrawArrays( GLenum mode, GLint first, GLsizei count );
void glDrawElements( GLenum mode, GLsizei count, GLenum type, const void *indices );
void glDrawRangeElements( GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices );

GLuint glCreateShader( GLenum type );
void glShaderSource( GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length );
void glCompileShader( GLuint shader );
void glGetShaderiv( GLuint shader, GLenum pname, GLint *params );
void glGetShaderInfoLog( GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog );
void glDeleteShader( GLuint shader );
GLuint glCreateProgram( void );
void glAttachShader( GLuint program, GLuint shader );
void glLinkProgram( GLuint program );
void glGetProgramiv( GLuint program, GLenum pname, GLint *params );
void glGetProgramInfoLog( GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog );
void glUseProgram( GLuint program );
void glDeleteProgram( GLuint program );
GLint glGetUniformLocation( GLuint program, const GLchar *name );
void glUniform1i( GLint location, GLint v0 );
void glUniform1f( GLint location, GLfloat v0 );
void glUniform2f( GLint location, GLfloat v0, GLfloat v1 );
void glUniform3f( GLint location, GLfloat v0, GLfloat v1, GLfloat v2 );
void glUniform4f( GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3 );
void glUniform1fv( GLint location, GLsizei count, const GLfloat *value );
void glUniform2fv( GLint location, GLsizei count, const GLfloat *value );
void glUniform3fv( GLint location, GLsizei count, const GLfloat *value );
void glUniform4fv( GLint location, GLsizei count, const GLfloat *value );
void glUniformMatrix2fv( GLint location, GLsizei count, GLboolean transpose, const GLfloat *value );
void glUniformMatrix3fv( GLint location, GLsizei count, GLboolean transpose, const GLfloat *value );
void glUniformMatrix4fv( GLint location, GLsizei count, GLboolean transpose, const GLfloat *value );

void glGenVertexArrays( GLsizei n, GLuint *arrays );
void glDeleteVertexArrays( GLsizei n, const GLuint *arrays );
void glBindVertexArray( GLuint array );

void glGenFramebuffers( GLsizei n, GLuint *framebuffers );
void glDeleteFramebuffers( GLsizei n, const GLuint *framebuffers );
void glBindFramebuffer( GLenum target, GLuint framebuffer );
void glFramebufferTexture2D( GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level );
void glFramebufferRenderbuffer( GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer );
GLenum glCheckFramebufferStatus( GLenum target );
void glBlitFramebuffer( GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter );
void glGenRenderbuffers( GLsizei n, GLuint *renderbuffers );
void glDeleteRenderbuffers( GLsizei n, const GLuint *renderbuffers );
void glBindRenderbuffer( GLenum target, GLuint renderbuffer );
void glRenderbufferStorage( GLenum target, GLenum internalformat, GLsizei width, GLsizei height );

void glReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels );

void glFlush( void );
void glFinish( void );

#ifdef __cplusplus
}
#endif
