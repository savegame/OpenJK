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

// ESSL 300 shader sources and the program cache for rd-gles3.
//
// G2: full generic stage coverage, ported from the rd-vulkan GLSL 450
// templates (shaders/glsl/gen_*.tmpl, fog_*.tmpl, light_*.tmpl, color.frag):
//   - SINGLE/MULTI (add/modulate)/BLEND2/BLEND3 stage combination, selected
//     at compile time via TEXMODE_n defines (tex_mode spec constant in vk)
//   - per-bundle vertex colors (USE_CL1/USE_CL2), identity (USE_CLX_IDENT)
//     and fixed-color (USE_FIXED_COLOR + u_FixedColor uniform) variants
//   - tcGen environment (USE_ENV, computed in the vertex shader)
//   - linear fog folded into stages (USE_FOG): fog texcoord in the vertex
//     shader, fog texture sample + ACFF modes in the fragment shader
//   - alpha test variants (GLS_ATEST_* -> ATEST_* defines)
//   - fog-only pass program and PMLIGHT dynamic light programs
//
// Programs are compiled lazily and cached; the cache key is
// (shader_type, fog_stage, atest, acff) which fully determines the
// define set. Blend/depth/cull state stays dynamic (gles3_set_state).

#include "tr_local.h"

typedef struct gles3_prog_s {
	GLuint		program;
	int			def_shader_type;
	int			def_fog_stage;
	int			def_atest;
	int			def_acff;
	gles3_uniform_locs_t locs;
} gles3_prog_t;

static gles3_prog_t g_progs[512];
static int g_progs_count;

static const char *const GLSL_VERSION = "#version 300 es\n";

// ---------------------------------------------------------------------------
// vertex shaders
// ---------------------------------------------------------------------------

// Generic stage vertex shader. Attribute layout matches gles3_commit_attribs
// in gles3_geometry.cpp.
static const char *const VS_GEN =
	"precision highp float;\n"
	"layout(location=0) in vec4 in_xyz;\n"
	"#if !defined(USE_FIXED_COLOR) && !defined(USE_CLX_IDENT)\n"
	"layout(location=1) in vec4 in_color0;\n"
	"#endif\n"
	"#ifndef USE_ENV\n"
	"layout(location=2) in vec2 in_st0;\n"
	"#endif\n"
	"#ifdef USE_TX1\n"
	"layout(location=3) in vec2 in_st1;\n"
	"#endif\n"
	"#ifdef USE_TX2\n"
	"layout(location=4) in vec2 in_st2;\n"
	"#endif\n"
	"#if defined(USE_ENV) || defined(USE_FOG)\n"
	"layout(location=5) in vec4 in_normal;\n"
	"#endif\n"
	"#ifdef USE_CL1\n"
	"layout(location=6) in vec4 in_color1;\n"
	"#endif\n"
	"#ifdef USE_CL2\n"
	"layout(location=7) in vec4 in_color2;\n"
	"#endif\n"
	"uniform mat4 u_MVP;\n"
	"#ifdef USE_ENV\n"
	"uniform vec4 u_EyePos;\n"
	"#endif\n"
	"#ifdef USE_FOG\n"
	"uniform vec4 u_FogDistanceVector;\n"
	"uniform vec4 u_FogDepthVector;\n"
	"uniform vec4 u_FogEyeT;\n"
	"#endif\n"
	"#if !defined(USE_FIXED_COLOR) && !defined(USE_CLX_IDENT)\n"
	"out vec4 v_color0;\n"
	"#endif\n"
	"#ifdef USE_CL1\n"
	"out vec4 v_color1;\n"
	"#endif\n"
	"#ifdef USE_CL2\n"
	"out vec4 v_color2;\n"
	"#endif\n"
	"out vec2 v_tc0;\n"
	"#ifdef USE_TX1\n"
	"out vec2 v_tc1;\n"
	"#endif\n"
	"#ifdef USE_TX2\n"
	"out vec2 v_tc2;\n"
	"#endif\n"
	"#ifdef USE_FOG\n"
	"out vec2 v_fogTc;\n"
	"#endif\n"
	"void main() {\n"
	"	vec3 position = in_xyz.xyz;\n"
	"	gl_Position = u_MVP * vec4(position, 1.0);\n"
	"#if !defined(USE_FIXED_COLOR) && !defined(USE_CLX_IDENT)\n"
	"	v_color0 = in_color0;\n"
	"#endif\n"
	"#ifdef USE_CL1\n"
	"	v_color1 = in_color1;\n"
	"#endif\n"
	"#ifdef USE_CL2\n"
	"	v_color2 = in_color2;\n"
	"#endif\n"
	"#ifdef USE_ENV\n"
	"	vec3 normal = in_normal.xyz;\n"
	"	vec3 viewer = normalize(u_EyePos.xyz - position);\n"
	"	float d = dot(normal, viewer);\n"
	"	vec2 reflected = normal.yz * 2.0 * d - viewer.yz;\n"
	"	v_tc0 = vec2(0.5 + reflected.x * 0.5, 0.5 - reflected.y * 0.5);\n"
	"#else\n"
	"	v_tc0 = in_st0;\n"
	"#endif\n"
	"#ifdef USE_TX1\n"
	"	v_tc1 = in_st1;\n"
	"#endif\n"
	"#ifdef USE_TX2\n"
	"	v_tc2 = in_st2;\n"
	"#endif\n"
	"#ifdef USE_FOG\n"
	"	float s = dot(position, u_FogDistanceVector.xyz) + u_FogDistanceVector.w;\n"
	"	float t = dot(position, u_FogDepthVector.xyz) + u_FogDepthVector.w;\n"
	"	if (u_FogEyeT.y == 1.0) {\n"
	"		if (t < 0.0) { t = 1.0 / 32.0; } else { t = 31.0 / 32.0; }\n"
	"	} else {\n"
	"		if (t < 1.0) { t = 1.0 / 32.0; }\n"
	"		else { t = 1.0 / 32.0 + (30.0 / 32.0 * t) / (t - u_FogEyeT.x); }\n"
	"	}\n"
	"	v_fogTc = vec2(s, t);\n"
	"#endif\n"
	"}\n";

// Fog-only pass vertex shader: position + fog texcoord (fog_vert.tmpl,
// USE_FOG_LINEAR, non-VBO path).
static const char *const VS_FOG =
	"precision highp float;\n"
	"layout(location=0) in vec4 in_xyz;\n"
	"uniform mat4 u_MVP;\n"
	"uniform vec4 u_FogDistanceVector;\n"
	"uniform vec4 u_FogDepthVector;\n"
	"uniform vec4 u_FogEyeT;\n"
	"out vec2 v_fogTc;\n"
	"void main() {\n"
	"	gl_Position = u_MVP * vec4(in_xyz.xyz, 1.0);\n"
	"	float s = dot(in_xyz.xyz, u_FogDistanceVector.xyz) + u_FogDistanceVector.w;\n"
	"	float t = dot(in_xyz.xyz, u_FogDepthVector.xyz) + u_FogDepthVector.w;\n"
	"	if (u_FogEyeT.y == 1.0) {\n"
	"		if (t < 0.0) { t = 1.0 / 32.0; } else { t = 31.0 / 32.0; }\n"
	"	} else {\n"
	"		if (t < 1.0) { t = 1.0 / 32.0; }\n"
	"		else { t = 1.0 / 32.0 + (30.0 / 32.0 * t) / (t - u_FogEyeT.x); }\n"
	"	}\n"
	"	v_fogTc = vec2(s, t);\n"
	"}\n";

// PMLIGHT dynamic light vertex shader (light_vert.tmpl, non-VBO path):
// streams object-space normal/light/view vectors for the fragment lighting.
static const char *const VS_LIGHT =
	"precision highp float;\n"
	"layout(location=0) in vec4 in_xyz;\n"
	"layout(location=2) in vec2 in_st0;\n"
	"layout(location=5) in vec4 in_normal;\n"
	"uniform mat4 u_MVP;\n"
	"uniform vec4 u_EyePos;\n"
	"uniform vec4 u_LightPos;\n"
	"#ifdef USE_FOG\n"
	"uniform vec4 u_FogDistanceVector;\n"
	"uniform vec4 u_FogDepthVector;\n"
	"uniform vec4 u_FogEyeT;\n"
	"#endif\n"
	"out vec2 v_tc0;\n"
	"out vec3 v_normal;\n"
	"out vec4 v_lightDir;\n"
	"out vec4 v_viewDir;\n"
	"#ifdef USE_FOG\n"
	"out vec2 v_fogTc;\n"
	"#endif\n"
	"void main() {\n"
	"	vec3 position = in_xyz.xyz;\n"
	"	gl_Position = u_MVP * vec4(position, 1.0);\n"
	"	v_tc0 = in_st0;\n"
	"	v_normal = in_normal.xyz;\n"
	"	v_lightDir = vec4(u_LightPos.xyz - position, 1.0);\n"
	"	v_viewDir = vec4(u_EyePos.xyz - position, 1.0);\n"
	"#ifdef USE_FOG\n"
	"	float s = dot(position, u_FogDistanceVector.xyz) + u_FogDistanceVector.w;\n"
	"	float t = dot(position, u_FogDepthVector.xyz) + u_FogDepthVector.w;\n"
	"	if (u_FogEyeT.y == 1.0) {\n"
	"		if (t < 0.0) { t = 1.0 / 32.0; } else { t = 31.0 / 32.0; }\n"
	"	} else {\n"
	"		if (t < 1.0) { t = 1.0 / 32.0; }\n"
	"		else { t = 1.0 / 32.0 + (30.0 / 32.0 * t) / (t - u_FogEyeT.x); }\n"
	"	}\n"
	"	v_fogTc = vec2(s, t);\n"
	"#endif\n"
	"}\n";

// ---------------------------------------------------------------------------
// fragment shaders
// ---------------------------------------------------------------------------

static const char *const FS_COLOR =
	"precision mediump float;\n"
	"out vec4 out_color;\n"
	"#if COLOR_MODE == 1\n"
	"void main() { out_color = vec4(1.0, 1.0, 1.0, 1.0); }\n"
	"#elif COLOR_MODE == 2\n"
	"void main() { out_color = vec4(0.2, 1.0, 0.2, 1.0); }\n"
	"#elif COLOR_MODE == 3\n"
	"void main() { out_color = vec4(1.0, 0.33, 0.2, 1.0); }\n"
	"#else\n"
	"void main() { out_color = vec4(0.0, 0.0, 0.0, 1.0); }\n"
	"#endif\n";

// Fog-only pass (fog_frag.tmpl USE_FOG_LINEAR). The frontend binds
// tr.fogImage to texture unit 0 for the fog pass.
static const char *const FS_FOG =
	"precision mediump float;\n"
	"uniform sampler2D u_Texture0;\n"
	"uniform vec4 u_FogColor;\n"
	"in vec2 v_fogTc;\n"
	"out vec4 out_color;\n"
	"void main() {\n"
	"	out_color = texture(u_Texture0, v_fogTc) * u_FogColor;\n"
	"}\n";

// PMLIGHT dynamic light fragment shader (light_frag.tmpl).
static const char *const FS_LIGHT =
	"precision highp float;\n"
	"uniform sampler2D u_Texture0;\n"
	"uniform vec4 u_LightColor;\n"
	"uniform vec4 u_LightVector;\n"
	"in vec2 v_tc0;\n"
	"in vec3 v_normal;\n"
	"in vec4 v_lightDir;\n"
	"in vec4 v_viewDir;\n"
	"#ifdef USE_FOG\n"
	"uniform sampler2D u_TextureFog;\n"
	"uniform vec4 u_FogColor;\n"
	"in vec2 v_fogTc;\n"
	"#endif\n"
	"out vec4 out_color;\n"
	"#ifdef ATEST_NEQ0\n"
	"uniform float u_AlphaTest;\n"
	"#endif\n"
	"#ifdef ATEST_LT80\n"
	"uniform float u_AlphaTest;\n"
	"#endif\n"
	"#ifdef ATEST_GE80\n"
	"uniform float u_AlphaTest;\n"
	"#endif\n"
	"void main() {\n"
	"	vec4 base = texture(u_Texture0, v_tc0);\n"
	"#ifdef ATEST_NEQ0\n"
	"	if (base.a == u_AlphaTest) discard;\n"
	"#endif\n"
	"#ifdef ATEST_LT80\n"
	"	if (base.a >= u_AlphaTest) discard;\n"
	"#endif\n"
	"#ifdef ATEST_GE80\n"
	"	if (base.a < u_AlphaTest) discard;\n"
	"#endif\n"
	"	vec4 lightColorRadius = u_LightColor;\n"
	"#ifdef USE_LINE\n"
	"	float scale = clamp(dot(-v_lightDir.xyz, u_LightVector.xyz) * u_LightVector.w, 0.0, 1.0);\n"
	"	vec4 LL = u_LightVector * scale + v_lightDir;\n"
	"	vec3 nL = normalize(LL.xyz);\n"
	"	vec3 nV = normalize(v_viewDir.xyz);\n"
	"	float intensFactor = 1.0 - (dot(LL.xyz, LL.xyz) * lightColorRadius.w);\n"
	"#else\n"
	"	vec3 nL = normalize(v_lightDir.xyz);\n"
	"	vec3 nV = normalize(v_viewDir.xyz);\n"
	"	float intensFactor = 1.0 - dot(v_lightDir.xyz, v_lightDir.xyz) * lightColorRadius.w;\n"
	"#endif\n"
	"	if (intensFactor <= 0.0) discard;\n"
	"	vec3 intens = lightColorRadius.rgb * intensFactor;\n"
	"#ifdef USE_FOG\n"
	"	vec4 fog = texture(u_TextureFog, v_fogTc);\n"
	"	base.xyz = base.xyz * (1.0 - fog.a);\n"
	"#endif\n"
	"	float diffuse = dot(v_normal, nL);\n"
	"	float specFactor = dot(v_normal, normalize(nL + nV));\n"
	"#ifdef ABS_LIGHT\n"
	"	if (diffuse * dot(v_normal, nV) <= 0.0) discard;\n"
	"	diffuse = abs(diffuse);\n"
	"	specFactor = abs(specFactor);\n"
	"#endif\n"
	"	vec4 spec = vec4(pow(specFactor, 10.0) * 0.25) * base * 0.8;\n"
	"	out_color = (base * vec4(diffuse) + spec) * vec4(intens, 1.0);\n"
	"}\n";

// Generic stage fragment shader, ported from gen_frag.tmpl (non-VBO path).
// Alpha test applies to color0 (the first bundle) exactly like the vk
// template; TEXMODE selects the multitexture combination at compile time.
static const char *const FS_GEN =
	"precision highp float;\n"
	"uniform sampler2D u_Texture0;\n"
	"#ifdef USE_TX1\n"
	"uniform sampler2D u_Texture1;\n"
	"#endif\n"
	"#ifdef USE_TX2\n"
	"uniform sampler2D u_Texture2;\n"
	"#endif\n"
	"#ifdef USE_FOG\n"
	"uniform sampler2D u_TextureFog;\n"
	"uniform vec4 u_FogColor;\n"
	"#endif\n"
	"#ifdef USE_FIXED_COLOR\n"
	"uniform vec4 u_FixedColor;\n"
	"#endif\n"
	"#if !defined(USE_FIXED_COLOR) && !defined(USE_CLX_IDENT)\n"
	"in vec4 v_color0;\n"
	"#endif\n"
	"#ifdef USE_CL1\n"
	"in vec4 v_color1;\n"
	"#endif\n"
	"#ifdef USE_CL2\n"
	"in vec4 v_color2;\n"
	"#endif\n"
	"in vec2 v_tc0;\n"
	"#ifdef USE_TX1\n"
	"in vec2 v_tc1;\n"
	"#endif\n"
	"#ifdef USE_TX2\n"
	"in vec2 v_tc2;\n"
	"#endif\n"
	"#ifdef USE_FOG\n"
	"in vec2 v_fogTc;\n"
	"#endif\n"
	"out vec4 out_color;\n"
	"#if defined(ATEST_NEQ0) || defined(ATEST_LT80) || defined(ATEST_GE80) || defined(ATEST_GEC0)\n"
	"uniform float u_AlphaTest;\n"
	"#endif\n"
	"void main() {\n"
	"#ifdef USE_FIXED_COLOR\n"
	"	vec4 color0 = texture(u_Texture0, v_tc0) * u_FixedColor;\n"
	"#elif defined(USE_CLX_IDENT)\n"
	"	vec4 color0 = texture(u_Texture0, v_tc0);\n"
	"#else\n"
	"	vec4 color0 = texture(u_Texture0, v_tc0) * v_color0;\n"
	"#endif\n"
	"#if defined(ATEST_NEQ0)\n"
	"	if (color0.a == u_AlphaTest) discard;\n"
	"#elif defined(ATEST_LT80)\n"
	"	if (color0.a >= u_AlphaTest) discard;\n"
	"#elif defined(ATEST_GE80)\n"
	"	if (color0.a < u_AlphaTest) discard;\n"
	"#elif defined(ATEST_GEC0)\n"
	"	if (color0.a < u_AlphaTest) discard;\n"
	"#endif\n"
	"	vec4 base;\n"
	"#if defined(USE_TX2)\n"
	"	// triple-texture stage\n"
	"	#if defined(USE_CL2)\n"
	"		#if TEXMODE == 1 || TEXMODE == 2\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			vec4 color2 = texture(u_Texture2, v_tc2) * v_color2;\n"
	"			base = vec4(color0.rgb + color1.rgb + color2.rgb, color0.a * color1.a * color2.a);\n"
	"		#elif TEXMODE == 3\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			vec4 color2 = texture(u_Texture2, v_tc2) * v_color2;\n"
	"			color0 *= color0.a; color1 *= color1.a; color2 *= color2.a;\n"
	"			base = vec4(color0.rgb + color1.rgb + color2.rgb, color0.a * color1.a * color2.a);\n"
	"		#elif TEXMODE == 4\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			vec4 color2 = texture(u_Texture2, v_tc2) * v_color2;\n"
	"			color0 *= 1.0 - color0.a; color1 *= 1.0 - color1.a; color2 *= 1.0 - color2.a;\n"
	"			base = vec4(color0.rgb + color1.rgb + color2.rgb, color0.a * color1.a * color2.a);\n"
	"		#elif TEXMODE == 5\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			vec4 color2 = texture(u_Texture2, v_tc2) * v_color2;\n"
	"			base = mix(mix(color0, color1, color1.a), color2, color2.a);\n"
	"		#elif TEXMODE == 6\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			vec4 color2 = texture(u_Texture2, v_tc2) * v_color2;\n"
	"			base = mix(color2, mix(color1, color0, color1.a), color2.a);\n"
	"		#elif TEXMODE == 7\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			vec4 color2 = texture(u_Texture2, v_tc2) * v_color2;\n"
	"			base = (color2 + color2.a) * (color1 + color1.a) * color0;\n"
	"		#else\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			vec4 color2 = texture(u_Texture2, v_tc2) * v_color2;\n"
	"			base = color0 * color1 * color2;\n"
	"		#endif\n"
	"	#else\n"
	"		// identity colors in color1/color2\n"
	"		#if TEXMODE == 1 || TEXMODE == 2\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1);\n"
	"			vec4 color2 = texture(u_Texture2, v_tc2);\n"
	"			base = vec4(color0.rgb + color1.rgb + color2.rgb, color0.a * color1.a * color2.a);\n"
	"		#else\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1);\n"
	"			vec4 color2 = texture(u_Texture2, v_tc2);\n"
	"			base = color0 * color1 * color2;\n"
	"		#endif\n"
	"	#endif\n"
	"#elif defined(USE_TX1)\n"
	"	// double-texture stage\n"
	"	#if defined(USE_CL1)\n"
	"		#if TEXMODE == 1 || TEXMODE == 2\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			base = vec4(color0.rgb + color1.rgb, color0.a * color1.a);\n"
	"		#elif TEXMODE == 3\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			color0 *= color0.a; color1 *= color1.a;\n"
	"			base = vec4(color0.rgb + color1.rgb, color0.a * color1.a);\n"
	"		#elif TEXMODE == 4\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			color0 *= 1.0 - color0.a; color1 *= 1.0 - color1.a;\n"
	"			base = vec4(color0.rgb + color1.rgb, color0.a * color1.a);\n"
	"		#elif TEXMODE == 5\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			base = mix(color0, color1, color1.a);\n"
	"		#elif TEXMODE == 6\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			base = mix(color1, color0, color1.a);\n"
	"		#elif TEXMODE == 7\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			base = (color1 + color1.a) * color0;\n"
	"		#else\n"
	"			vec4 color1 = texture(u_Texture1, v_tc1) * v_color1;\n"
	"			base = color0 * color1;\n"
	"		#endif\n"
	"	#else\n"
	"		// identity color1\n"
	"		#if defined(USE_FIXED_COLOR)\n"
	"			#if TEXMODE == 1 || TEXMODE == 2\n"
	"				vec4 color1 = texture(u_Texture1, v_tc1) * u_FixedColor;\n"
	"				base = vec4(color0.rgb + color1.rgb, color0.a * color1.a);\n"
	"			#else\n"
	"				vec4 color1 = texture(u_Texture1, v_tc1) * u_FixedColor;\n"
	"				base = color0 * color1;\n"
	"			#endif\n"
	"		#elif defined(USE_CLX_IDENT)\n"
	"			#if TEXMODE == 1 || TEXMODE == 2\n"
	"				vec4 color1 = texture(u_Texture1, v_tc1);\n"
	"				base = vec4(color0.rgb + color1.rgb, color0.a * color1.a);\n"
	"			#else\n"
	"				vec4 color1 = texture(u_Texture1, v_tc1);\n"
	"				base = color0 * color1;\n"
	"			#endif\n"
	"		#else\n"
	"			// color1 modulated by color0 (per-vertex), like the vk template\n"
	"			#if TEXMODE == 1\n"
	"				vec4 color1 = texture(u_Texture1, v_tc1);\n"
	"				base = vec4(color0.rgb + color1.rgb, color0.a * color1.a);\n"
	"			#elif TEXMODE == 2\n"
	"				vec4 color1 = texture(u_Texture1, v_tc1) * v_color0;\n"
	"				base = vec4(color0.rgb + color1.rgb, color0.a * color1.a);\n"
	"			#else\n"
	"				vec4 color1 = texture(u_Texture1, v_tc1);\n"
	"				base = color0 * color1;\n"
	"			#endif\n"
	"		#endif\n"
	"	#endif\n"
	"#else\n"
	"	base = color0;\n"
	"#endif\n"
	"#ifdef USE_FOG\n"
	"	vec4 fog = texture(u_TextureFog, v_fogTc);\n"
	"	vec4 fog_color = fog * u_FogColor;\n"
	"	#if defined(ACFF_MODULATE_RGB)\n"
	"		base.rgb *= (1.0 - fog.a);\n"
	"	#elif defined(ACFF_MODULATE_RGBA)\n"
	"		base *= (1.0 - fog.a);\n"
	"	#elif defined(ACFF_MODULATE_ALPHA)\n"
	"		base.a *= (1.0 - fog.a);\n"
	"	#else\n"
	"		base = mix(base, fog_color, fog.a);\n"
	"	#endif\n"
	"#endif\n"
	"	out_color = base;\n"
	"}\n";

// Depth-fragment variant (TYPE_SINGLE_TEXTURE_DF): ES 3.0 has no
// gl_FragDepth, so the backend renders these stages depth-only
// (glColorMask off) with an alpha-test cutout, approximating gen0_df.
static const char *const FS_DF =
	"precision mediump float;\n"
	"uniform sampler2D u_Texture0;\n"
	"in vec2 v_tc0;\n"
	"out vec4 out_color;\n"
	"void main() {\n"
	"	vec4 color0 = texture(u_Texture0, v_tc0);\n"
	"	if (color0.a < 0.75) discard;\n"
	"	out_color = vec4(0.0);\n"
	"}\n";

// ---------------------------------------------------------------------------
// compile & cache
// ---------------------------------------------------------------------------

static GLuint gles3_compile_shader( GLenum type, const char *src, const char *defines )
{
	char buf[16384];
	const GLchar *src_ptr;
	GLuint shader;
	GLint status;

	Com_sprintf( buf, sizeof(buf), "%s%s%s", GLSL_VERSION, defines ? defines : "", src );

	shader = glCreateShader( type );
	src_ptr = buf;
	glShaderSource( shader, 1, &src_ptr, NULL );
	glCompileShader( shader );
	glGetShaderiv( shader, GL_COMPILE_STATUS, &status );
	if ( status == 0 ) {
		char log[2048];
		glGetShaderInfoLog( shader, sizeof(log), NULL, log );
		ri.Error( ERR_FATAL, "gles3: shader compile failed:\n%s\n%s", buf, log );
	}
	return shader;
}

// Resolve every uniform location once, right after linking, instead of
// asking the driver for it on every draw call (gles3_apply_uniforms used to
// call glGetUniformLocation ~15 times plus glGetIntegerv(GL_CURRENT_PROGRAM)
// per draw - cheap on desktop Mesa, a synchronous driver round trip on
// mobile GLES3 drivers, and the prime suspect for menu slowdowns on device).
static void gles3_resolve_uniform_locs( GLuint program, gles3_uniform_locs_t *locs )
{
	locs->u_MVP				= glGetUniformLocation( program, "u_MVP" );
	locs->u_ModelMatrix		= glGetUniformLocation( program, "u_ModelMatrix" );
	locs->u_EyePos			= glGetUniformLocation( program, "u_EyePos" );
	locs->u_LightPos		= glGetUniformLocation( program, "u_LightPos" );
	locs->u_LightColor		= glGetUniformLocation( program, "u_LightColor" );
	locs->u_LightVector		= glGetUniformLocation( program, "u_LightVector" );
	locs->u_FogDistanceVector = glGetUniformLocation( program, "u_FogDistanceVector" );
	locs->u_FogDepthVector	= glGetUniformLocation( program, "u_FogDepthVector" );
	locs->u_FogEyeT			= glGetUniformLocation( program, "u_FogEyeT" );
	locs->u_FogColor		= glGetUniformLocation( program, "u_FogColor" );
	locs->u_Texture0		= glGetUniformLocation( program, "u_Texture0" );
	locs->u_Texture1		= glGetUniformLocation( program, "u_Texture1" );
	locs->u_Texture2		= glGetUniformLocation( program, "u_Texture2" );
	locs->u_TextureFog		= glGetUniformLocation( program, "u_TextureFog" );
	locs->u_FixedColor		= glGetUniformLocation( program, "u_FixedColor" );
	locs->u_AlphaTest		= glGetUniformLocation( program, "u_AlphaTest" );
}

static GLuint gles3_link_program( const char *vs_src, const char *vs_defines, const char *fs_src, const char *fs_defines )
{
	GLuint vs, fs, program;
	GLint status;

	vs = gles3_compile_shader( GL_VERTEX_SHADER, vs_src, vs_defines );
	fs = gles3_compile_shader( GL_FRAGMENT_SHADER, fs_src, fs_defines );

	program = glCreateProgram();
	glAttachShader( program, vs );
	glAttachShader( program, fs );
	glLinkProgram( program );
	glGetProgramiv( program, GL_LINK_STATUS, &status );
	if ( status == 0 ) {
		char log[2048];
		glGetProgramInfoLog( program, sizeof(log), NULL, log );
		ri.Error( ERR_FATAL, "gles3: program link failed: %s", log );
	}

	glDeleteShader( vs );
	glDeleteShader( fs );
	return program;
}

void gles3_init_programs( void )
{
	g_progs_count = 0;
}

void gles3_destroy_programs( void )
{
	int i;
	for ( i = 0; i < g_progs_count; i++ ) {
		glDeleteProgram( g_progs[i].program );
	}
	g_progs_count = 0;
}

// Build the define string for the generic program variants.
static void gles3_gen_defines( const Vk_Pipeline_Def *def, const char **vs_defines, const char **fs_defines )
{
	static char vs_buf[1024], fs_buf[1024];
	const char *texmode = "";
	int tex_count = 0, color_count = 0, env = 0, fog = 0;
	qboolean ident = qfalse, fixed = qfalse;

	*vs_buf = '\0';
	*fs_buf = '\0';

	switch ( def->shader_type ) {
		case TYPE_SINGLE_TEXTURE_IDENTITY:
		case TYPE_SINGLE_TEXTURE_IDENTITY_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY:
		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY_ENV:
		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY:
		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY_ENV:
			ident = qtrue;
			break;
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR:
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR:
		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR_ENV:
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR:
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR_ENV:
			fixed = qtrue;
			break;
		default:
			break;
	}

	switch ( def->shader_type ) {
		case TYPE_SINGLE_TEXTURE:					break;
		case TYPE_SINGLE_TEXTURE_ENV:				env = 1; break;
		case TYPE_SINGLE_TEXTURE_IDENTITY:			ident = qtrue; break;
		case TYPE_SINGLE_TEXTURE_IDENTITY_ENV:		ident = qtrue; env = 1; break;
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR:		fixed = qtrue; break;
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR_ENV:	fixed = qtrue; env = 1; break;
		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY:		tex_count = 2; texmode = "1"; break;
		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY_ENV:	tex_count = 2; texmode = "1"; env = 1; break;
		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY:		tex_count = 2; texmode = "0"; break;
		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY_ENV:	tex_count = 2; texmode = "0"; env = 1; break;
		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR:	tex_count = 2; texmode = "2"; break;
		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR_ENV: tex_count = 2; texmode = "2"; env = 1; break;
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR:	tex_count = 2; texmode = "0"; break;
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR_ENV: tex_count = 2; texmode = "0"; env = 1; break;
		case TYPE_MULTI_TEXTURE_MUL2:				tex_count = 2; texmode = "0"; break;
		case TYPE_MULTI_TEXTURE_MUL2_ENV:			tex_count = 2; texmode = "0"; env = 1; break;
		case TYPE_MULTI_TEXTURE_ADD2_1_1:			tex_count = 2; texmode = "1"; break;
		case TYPE_MULTI_TEXTURE_ADD2_1_1_ENV:		tex_count = 2; texmode = "1"; env = 1; break;
		case TYPE_MULTI_TEXTURE_ADD2:				tex_count = 2; texmode = "2"; break;
		case TYPE_MULTI_TEXTURE_ADD2_ENV:			tex_count = 2; texmode = "2"; env = 1; break;
		case TYPE_MULTI_TEXTURE_MUL3:				tex_count = 3; texmode = "0"; break;
		case TYPE_MULTI_TEXTURE_MUL3_ENV:			tex_count = 3; texmode = "0"; env = 1; break;
		case TYPE_MULTI_TEXTURE_ADD3_1_1:			tex_count = 3; texmode = "1"; break;
		case TYPE_MULTI_TEXTURE_ADD3_1_1_ENV:		tex_count = 3; texmode = "1"; env = 1; break;
		case TYPE_MULTI_TEXTURE_ADD3:				tex_count = 3; texmode = "2"; break;
		case TYPE_MULTI_TEXTURE_ADD3_ENV:			tex_count = 3; texmode = "2"; env = 1; break;
		case TYPE_BLEND2_ADD:						tex_count = 2; color_count = 1; texmode = "2"; break;
		case TYPE_BLEND2_ADD_ENV:					tex_count = 2; color_count = 1; texmode = "2"; env = 1; break;
		case TYPE_BLEND2_MUL:						tex_count = 2; color_count = 1; texmode = "0"; break;
		case TYPE_BLEND2_MUL_ENV:					tex_count = 2; color_count = 1; texmode = "0"; env = 1; break;
		case TYPE_BLEND2_ALPHA:						tex_count = 2; color_count = 1; texmode = "3"; break;
		case TYPE_BLEND2_ALPHA_ENV:					tex_count = 2; color_count = 1; texmode = "3"; env = 1; break;
		case TYPE_BLEND2_ONE_MINUS_ALPHA:			tex_count = 2; color_count = 1; texmode = "4"; break;
		case TYPE_BLEND2_ONE_MINUS_ALPHA_ENV:		tex_count = 2; color_count = 1; texmode = "4"; env = 1; break;
		case TYPE_BLEND2_MIX_ALPHA:					tex_count = 2; color_count = 1; texmode = "5"; break;
		case TYPE_BLEND2_MIX_ALPHA_ENV:				tex_count = 2; color_count = 1; texmode = "5"; env = 1; break;
		case TYPE_BLEND2_MIX_ONE_MINUS_ALPHA:		tex_count = 2; color_count = 1; texmode = "6"; break;
		case TYPE_BLEND2_MIX_ONE_MINUS_ALPHA_ENV:	tex_count = 2; color_count = 1; texmode = "6"; env = 1; break;
		case TYPE_BLEND2_DST_COLOR_SRC_ALPHA:		tex_count = 2; color_count = 1; texmode = "7"; break;
		case TYPE_BLEND2_DST_COLOR_SRC_ALPHA_ENV:	tex_count = 2; color_count = 1; texmode = "7"; env = 1; break;
		case TYPE_BLEND3_ADD:						tex_count = 3; color_count = 2; texmode = "2"; break;
		case TYPE_BLEND3_ADD_ENV:					tex_count = 3; color_count = 2; texmode = "2"; env = 1; break;
		case TYPE_BLEND3_MUL:						tex_count = 3; color_count = 2; texmode = "0"; break;
		case TYPE_BLEND3_MUL_ENV:					tex_count = 3; color_count = 2; texmode = "0"; env = 1; break;
		case TYPE_BLEND3_ALPHA:						tex_count = 3; color_count = 2; texmode = "3"; break;
		case TYPE_BLEND3_ALPHA_ENV:					tex_count = 3; color_count = 2; texmode = "3"; env = 1; break;
		case TYPE_BLEND3_ONE_MINUS_ALPHA:			tex_count = 3; color_count = 2; texmode = "4"; break;
		case TYPE_BLEND3_ONE_MINUS_ALPHA_ENV:		tex_count = 3; color_count = 2; texmode = "4"; env = 1; break;
		case TYPE_BLEND3_MIX_ALPHA:					tex_count = 3; color_count = 2; texmode = "5"; break;
		case TYPE_BLEND3_MIX_ALPHA_ENV:				tex_count = 3; color_count = 2; texmode = "5"; env = 1; break;
		case TYPE_BLEND3_MIX_ONE_MINUS_ALPHA:		tex_count = 3; color_count = 2; texmode = "6"; break;
		case TYPE_BLEND3_MIX_ONE_MINUS_ALPHA_ENV:	tex_count = 3; color_count = 2; texmode = "6"; env = 1; break;
		case TYPE_BLEND3_DST_COLOR_SRC_ALPHA:		tex_count = 3; color_count = 2; texmode = "7"; break;
		case TYPE_BLEND3_DST_COLOR_SRC_ALPHA_ENV:	tex_count = 3; color_count = 2; texmode = "7"; env = 1; break;
		default:
			break;
	}

	if ( tex_count >= 2 )	Q_strcat( vs_buf, sizeof(vs_buf), "#define USE_TX1\n" );
	if ( tex_count >= 3 )	Q_strcat( vs_buf, sizeof(vs_buf), "#define USE_TX2\n" );
	if ( color_count >= 1 )	Q_strcat( vs_buf, sizeof(vs_buf), "#define USE_CL1\n" );
	if ( color_count >= 2 )	Q_strcat( vs_buf, sizeof(vs_buf), "#define USE_CL2\n" );
	if ( env )				Q_strcat( vs_buf, sizeof(vs_buf), "#define USE_ENV\n" );
	if ( def->fog_stage )	Q_strcat( vs_buf, sizeof(vs_buf), "#define USE_FOG\n" );
	if ( ident )			Q_strcat( vs_buf, sizeof(vs_buf), "#define USE_CLX_IDENT\n" );
	if ( fixed )			Q_strcat( vs_buf, sizeof(vs_buf), "#define USE_FIXED_COLOR\n" );

	Com_sprintf( fs_buf, sizeof(fs_buf), "%s", vs_buf );
	if ( tex_count >= 2 ) {
		char tm[32];
		Com_sprintf( tm, sizeof(tm), "#define TEXMODE %s\n", texmode );
		Q_strcat( fs_buf, sizeof(fs_buf), tm );
	}
	switch ( def->state_bits & GLS_ATEST_BITS ) {
		case GLS_ATEST_GT_0:	Q_strcat( fs_buf, sizeof(fs_buf), "#define ATEST_NEQ0\n" ); break;
		case GLS_ATEST_LT_80:	Q_strcat( fs_buf, sizeof(fs_buf), "#define ATEST_LT80\n" ); break;
		case GLS_ATEST_GE_80:	Q_strcat( fs_buf, sizeof(fs_buf), "#define ATEST_GE80\n" ); break;
		case GLS_ATEST_GE_C0:	Q_strcat( fs_buf, sizeof(fs_buf), "#define ATEST_GEC0\n" ); break;
		default: break;
	}
	if ( def->fog_stage ) {
		switch ( def->acff ) {
			case ACFF_MODULATE_RGB:		Q_strcat( fs_buf, sizeof(fs_buf), "#define ACFF_MODULATE_RGB\n" ); break;
			case ACFF_MODULATE_RGBA:	Q_strcat( fs_buf, sizeof(fs_buf), "#define ACFF_MODULATE_RGBA\n" ); break;
			case ACFF_MODULATE_ALPHA:	Q_strcat( fs_buf, sizeof(fs_buf), "#define ACFF_MODULATE_ALPHA\n" ); break;
			default: break;
		}
	}

	*vs_defines = vs_buf;
	*fs_defines = fs_buf;
}

// Returns a cached program for the shader class of def.  The cache key covers
// the shader type and the variant bits (fog stage, alpha test, acff); blend
// funcs, depth, cull are dynamic GL state applied by gles3_set_state.
GLuint gles3_get_program( const Vk_Pipeline_Def *def, const gles3_uniform_locs_t **out_locs )
{
	const char *vs_src = VS_GEN, *fs_src = FS_GEN;
	const char *vs_defines = "", *fs_defines = "";
	char extra[256];
	int atest = (int)( def->state_bits & GLS_ATEST_BITS );
	int i;

	*extra = '\0';

	switch ( def->shader_type ) {
		case TYPE_COLOR_BLACK:
		case TYPE_COLOR_WHITE:
		case TYPE_COLOR_GREEN:
		case TYPE_COLOR_RED:
		case TYPE_DOT:
			vs_src = VS_GEN;
			fs_src = FS_COLOR;
			if ( def->shader_type == TYPE_COLOR_WHITE )
				Q_strcat( extra, sizeof(extra), "#define COLOR_MODE 1\n" );
			else if ( def->shader_type == TYPE_COLOR_GREEN )
				Q_strcat( extra, sizeof(extra), "#define COLOR_MODE 2\n" );
			else if ( def->shader_type == TYPE_COLOR_RED )
				Q_strcat( extra, sizeof(extra), "#define COLOR_MODE 3\n" );
			else
				Q_strcat( extra, sizeof(extra), "#define COLOR_MODE 0\n" );
			fs_defines = extra;
			// color programs read no attributes beyond xyz
			break;

		case TYPE_FOG_ONLY:
			vs_src = VS_FOG;
			fs_src = FS_FOG;
			vs_defines = "";
			fs_defines = "";
			break;

		case TYPE_SINGLE_TEXTURE_LIGHTING:
		case TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR:
			vs_src = VS_LIGHT;
			fs_src = FS_LIGHT;
			if ( def->shader_type == TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR )
				Q_strcat( extra, sizeof(extra), "#define USE_LINE\n" );
			if ( def->abs_light )
				Q_strcat( extra, sizeof(extra), "#define ABS_LIGHT\n" );
			if ( def->fog_stage )
				Q_strcat( extra, sizeof(extra), "#define USE_FOG\n" );
			switch ( atest ) {
				case GLS_ATEST_GT_0:	Q_strcat( extra, sizeof(extra), "#define ATEST_NEQ0\n" ); break;
				case GLS_ATEST_LT_80:	Q_strcat( extra, sizeof(extra), "#define ATEST_LT80\n" ); break;
				case GLS_ATEST_GE_80:	Q_strcat( extra, sizeof(extra), "#define ATEST_GE80\n" ); break;
				default: break;
			}
			vs_defines = extra;
			fs_defines = extra;
			break;

		case TYPE_SINGLE_TEXTURE_DF:
			vs_src = VS_GEN;
			fs_src = FS_DF;
			vs_defines = "#define USE_CLX_IDENT\n";
			fs_defines = "";
			break;

		default:
			gles3_gen_defines( def, &vs_defines, &fs_defines );
			break;
	}

	for ( i = 0; i < g_progs_count; i++ ) {
		if ( g_progs[i].def_shader_type == (int)def->shader_type &&
			 g_progs[i].def_fog_stage == (int)def->fog_stage &&
			 g_progs[i].def_atest == atest &&
			 g_progs[i].def_acff == (int)def->acff ) {
			if ( out_locs )
				*out_locs = &g_progs[i].locs;
			return g_progs[i].program;
		}
	}

	if ( g_progs_count >= (int)ARRAY_LEN( g_progs ) ) {
		ri.Error( ERR_FATAL, "gles3: program cache exhausted" );
	}

	g_progs[g_progs_count].program = gles3_link_program( vs_src, vs_defines, fs_src, fs_defines );
	g_progs[g_progs_count].def_shader_type = (int)def->shader_type;
	g_progs[g_progs_count].def_fog_stage = (int)def->fog_stage;
	g_progs[g_progs_count].def_atest = atest;
	g_progs[g_progs_count].def_acff = (int)def->acff;
	gles3_resolve_uniform_locs( g_progs[g_progs_count].program, &g_progs[g_progs_count].locs );
	g_progs_count++;

	if ( out_locs )
		*out_locs = &g_progs[g_progs_count - 1].locs;
	return g_progs[g_progs_count - 1].program;
}
