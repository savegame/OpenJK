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
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#pragma once

// rd-gles3 (AuroraOS): OpenGL ES 3.0 core only.
// Fixed-function GL 1.x entry points (glBegin/glMatrixMode/glTexEnv/...)
// are deliberately NOT mapped here; every call site that used them must be
// ported or stubbed (see research/gles3_renderer_plan.md, milestones M2..M7).

#include <GLES3/gl3.h>

// compatibility constants formerly provided by glext.h / desktop gl.h
#ifndef GL_CLAMP
#define GL_CLAMP GL_CLAMP_TO_EDGE
#endif
#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB GL_TEXTURE0
#endif
#ifndef GL_TEXTURE1_ARB
#define GL_TEXTURE1_ARB GL_TEXTURE1
#endif
#ifndef GL_TEXTURE2_ARB
#define GL_TEXTURE2_ARB GL_TEXTURE2
#endif
#ifndef GL_TEXTURE3_ARB
#define GL_TEXTURE3_ARB GL_TEXTURE3
#endif
#ifndef GL_MAX_TEXTURE_UNITS_ARB
#define GL_MAX_TEXTURE_UNITS_ARB GL_MAX_TEXTURE_IMAGE_UNITS
#endif
#ifndef GL_RGB5
#define GL_RGB5 GL_RGB565
#endif
// S3TC internal formats: texture compression is disabled in this renderer
// (plan section 5), these are only referenced from unreachable switch arms.
#ifndef GL_RGB4_S3TC
#define GL_RGB4_S3TC 0x83A0
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
// Fixed-function texture-env tokens kept as opaque identifiers: in ES3 the
// stage combination lives in the shader pipeline def (TODO(M3)); they must
// only be compared against each other, never passed to GL.
#ifndef GL_MODULATE
#define GL_MODULATE 0x2100
#endif
#ifndef GL_ADD
#define GL_ADD 0x0104
#endif
#ifndef GL_DECAL
#define GL_DECAL 0x2101
#endif
#ifndef GL_REPLACE
#define GL_REPLACE 0x1D01
#endif

#define qglActiveTextureARB glActiveTexture
#define qglBindTexture glBindTexture
#define qglBlendFunc glBlendFunc
#define qglClear glClear
#define qglClearColor glClearColor
#define qglClearDepth glClearDepthf
#define qglClearStencil glClearStencil
#define qglColorMask glColorMask
#define qglCopyTexImage2D glCopyTexImage2D
#define qglCopyTexSubImage2D glCopyTexSubImage2D
#define qglCullFace glCullFace
#define qglDeleteTextures glDeleteTextures
#define qglDepthFunc glDepthFunc
#define qglDepthMask glDepthMask
#define qglDepthRange glDepthRangef
#define qglDisable glDisable
#define qglDrawArrays glDrawArrays
#define qglDrawElements glDrawElements
#define qglEnable glEnable
#define qglFinish glFinish
#define qglFlush glFlush
#define qglFrontFace glFrontFace
#define qglGenTextures glGenTextures
#define qglGetBooleanv glGetBooleanv
#define qglGetError glGetError
#define qglGetFloatv glGetFloatv
#define qglGetIntegerv glGetIntegerv
#define qglGetString glGetString
#define qglGetTexParameterfv glGetTexParameterfv
#define qglGetTexParameteriv glGetTexParameteriv
#define qglHint glHint
#define qglIsEnabled glIsEnabled
#define qglIsTexture glIsTexture
#define qglLineWidth glLineWidth
#define qglPixelStorei glPixelStorei
#define qglPolygonOffset glPolygonOffset
#define qglReadPixels glReadPixels
#define qglScissor glScissor
#define qglStencilFunc glStencilFunc
#define qglStencilMask glStencilMask
#define qglStencilOp glStencilOp
#define qglStencilOpSeparate glStencilOpSeparate
#define qglTexImage2D glTexImage2D
#define qglTexParameterf glTexParameterf
#define qglTexParameterfv glTexParameterfv
#define qglTexParameteri glTexParameteri
#define qglTexParameteriv glTexParameteriv
#define qglTexSubImage2D glTexSubImage2D
#define qglViewport glViewport
