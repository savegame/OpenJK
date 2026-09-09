#============================================================================
# Copyright (C) 2013 - 2018, OpenJK contributors
#
# This file is part of the OpenJK source code.
#
# OpenJK is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with OpenJK; if not, see <http://www.gnu.org/licenses/>.
#============================================================================

# Make sure the user is not executing this script directly
if(NOT InOpenJK)
	message(FATAL_ERROR "Use the top-level cmake script!")
endif(NOT InOpenJK)

if(BuildSPRdGLES3)

	set(SPRDGLES3RendererIncludeDirectories
		${SharedDir}
		${SPDir}
		"${SPDir}/rd-gles3"
		"${GSLIncludeDirectory}"
		)

	# Defines
	set(SPRDGLES3Defines ${SharedDefines} "RENDERER" "_JK2EXE" ${AuroraDefines})

	# GHOUL 2 (SP versions, shared with rd-vulkan semantics)
	set(SPRDGLES3G2Files
		"${SPDir}/ghoul2/G2.h"
		"${SPDir}/ghoul2/ghoul2_gore.h"
		"${SPDir}/rd-gles3/G2_API.cpp"
		"${SPDir}/rd-gles3/G2_bolts.cpp"
		"${SPDir}/rd-gles3/G2_bones.cpp"
		"${SPDir}/rd-gles3/G2_misc.cpp"
		"${SPDir}/rd-gles3/G2_surfaces.cpp"
		)
	source_group("ghoul2" FILES ${SPRDGLES3G2Files})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3G2Files})

	# main renderer files (forked from SP rd-vulkan, backend replaced with GLES3)
	set(SPRDGLES3MainFiles
		"${SPDir}/rd-gles3/tr_backend.cpp"
		"${SPDir}/rd-gles3/tr_bsp.cpp"
		"${SPDir}/rd-gles3/tr_cache.cpp"
		"${SPDir}/rd-gles3/tr_cache.h"
		"${SPDir}/rd-gles3/tr_cmds.cpp"
		"${SPDir}/rd-gles3/tr_curve.cpp"
		"${SPDir}/rd-gles3/tr_decals.cpp"
		"${SPDir}/rd-gles3/tr_draw.cpp"
		"${SPDir}/rd-gles3/tr_ghoul2.cpp"
		"${SPDir}/rd-gles3/tr_image.cpp"
		"${SPDir}/rd-gles3/tr_init.cpp"
		"${SPDir}/rd-gles3/tr_light.cpp"
		"${SPDir}/rd-gles3/tr_local.h"
		"${SPDir}/rd-gles3/tr_main.cpp"
		"${SPDir}/rd-gles3/tr_marks.cpp"
		"${SPDir}/rd-gles3/tr_mesh.cpp"
		"${SPDir}/rd-gles3/tr_model.cpp"
		"${SPDir}/rd-gles3/tr_quicksprite.cpp"
		"${SPDir}/rd-gles3/tr_quicksprite.h"
		"${SPDir}/rd-gles3/tr_scene.cpp"
		"${SPDir}/rd-gles3/tr_shade.cpp"
		"${SPDir}/rd-gles3/tr_shade_calc.cpp"
		"${SPDir}/rd-gles3/tr_shader.cpp"
		"${SPDir}/rd-gles3/tr_shadows.cpp"
		"${SPDir}/rd-gles3/tr_skin.cpp"
		"${SPDir}/rd-gles3/tr_sky.cpp"
		"${SPDir}/rd-gles3/tr_stl.cpp"
		"${SPDir}/rd-gles3/tr_stl.h"
		"${SPDir}/rd-gles3/tr_subs.cpp"
		"${SPDir}/rd-gles3/tr_surface.cpp"
		"${SPDir}/rd-gles3/tr_surfacesprites.cpp"
		"${SPDir}/rd-gles3/tr_world.cpp"
		"${SPDir}/rd-gles3/tr_WorldEffects.cpp"
		"${SPDir}/rd-gles3/tr_WorldEffects.h"
		"${SPDir}/rd-gles3/gles3_api.h"
		"${SPDir}/rd-gles3/gles3_local.h"
		"${SPDir}/rd-gles3/gles3_init.cpp"
		"${SPDir}/rd-gles3/gles3_frame.cpp"
		"${SPDir}/rd-gles3/gles3_geometry.cpp"
		"${SPDir}/rd-gles3/gles3_image.cpp"
		"${SPDir}/rd-gles3/gles3_pipelines.cpp"
		"${SPDir}/rd-gles3/gles3_shaders.cpp"
		"${SPDir}/rd-gles3/gles3_info.cpp"
		"${SPDir}/rd-gles3/gles3_debug.cpp"
		"${SPDir}/rd-gles3/gles3_flares.cpp"
		"${SPDir}/rd-gles3/gles3_stubs.cpp"
		"${SPDir}/rd-gles3/vk_image_process.cpp"
		"${SPDir}/rd-gles3/vk_local.h"
		)
	source_group("renderer" FILES ${SPRDGLES3MainFiles})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3MainFiles})

	# shader templates (GLSL 450 sources, reference for later milestone conversions;
	# G1 uses inline ESSL 300 strings in gles3_shaders.cpp)
	set(SPRDGLES3ShaderAssets
		"${SPDir}/rd-gles3/shaders/glsl/blend.frag"
		"${SPDir}/rd-gles3/shaders/glsl/bloom.frag"
		"${SPDir}/rd-gles3/shaders/glsl/blur.frag"
		"${SPDir}/rd-gles3/shaders/glsl/color.frag"
		"${SPDir}/rd-gles3/shaders/glsl/color.vert"
		"${SPDir}/rd-gles3/shaders/glsl/dot.frag"
		"${SPDir}/rd-gles3/shaders/glsl/dot.vert"
		"${SPDir}/rd-gles3/shaders/glsl/fog_frag.tmpl"
		"${SPDir}/rd-gles3/shaders/glsl/fog_vert.tmpl"
		"${SPDir}/rd-gles3/shaders/glsl/gamma.frag"
		"${SPDir}/rd-gles3/shaders/glsl/gamma.vert"
		"${SPDir}/rd-gles3/shaders/glsl/gen_frag.tmpl"
		"${SPDir}/rd-gles3/shaders/glsl/gen_vert.tmpl"
		"${SPDir}/rd-gles3/shaders/glsl/light_frag.tmpl"
		"${SPDir}/rd-gles3/shaders/glsl/light_vert.tmpl"
		"${SPDir}/rd-gles3/shaders/glsl/refraction.frag"
		"${SPDir}/rd-gles3/shaders/glsl/refraction.tmpl"
		"${SPDir}/rd-gles3/shaders/glsl/surface_sprite_frag.tmpl"
		"${SPDir}/rd-gles3/shaders/glsl/surface_sprite_vert.tmpl"
		)
	source_group("renderer\\shaders" FILES ${SPRDGLES3ShaderAssets})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3ShaderAssets})

	set(SPRDGLES3Utils
		"${SPDir}/rd-gles3/utils/stb_dxt.h")
	source_group("renderer\\utils" FILES ${SPRDGLES3Utils})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3Utils})

	set(SPRDGLES3RdCommonFiles
		"${SPDir}/rd-common/mdx_format.h"
		"${SPDir}/rd-common/tr_common.h"
		"${SPDir}/rd-common/tr_font.cpp"
		"${SPDir}/rd-common/tr_font.h"
		"${SPDir}/rd-common/tr_image_load.cpp"
		"${SPDir}/rd-common/tr_image_jpg.cpp"
		"${SPDir}/rd-common/tr_image_tga.cpp"
		"${SPDir}/rd-common/tr_image_png.cpp"
		"${SPDir}/rd-common/tr_noise.cpp"
		"${SPDir}/rd-common/tr_public.h"
		"${SPDir}/rd-common/tr_types.h")
	source_group("rd-common" FILES ${SPRDGLES3RdCommonFiles})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3RdCommonFiles})

	set(SPRDGLES3CommonFiles
		"${SPDir}/qcommon/matcomp.cpp"
		"${SPDir}/qcommon/q_shared.cpp"

		${SharedCommonFiles})
	source_group("common" FILES ${SPRDGLES3CommonFiles})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3CommonFiles})

	set(SPRDGLES3CommonSafeFiles
		${SharedCommonSafeFiles}
		)
	source_group("common/safe" FILES ${SPRDGLES3CommonSafeFiles})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3CommonSafeFiles})

	# Transparently use either bundled or system jpeg.
	list(APPEND SPRDGLES3RendererIncludeDirectories ${JPEG_INCLUDE_DIR})
	list(APPEND SPRDGLES3RendererLibraries          ${JPEG_LIBRARIES})

	# Transparently use either bundled or system libpng.  Order is important --
	# libpng used zlib, so it must come before it on the linker command line.
	list(APPEND SPRDGLES3RendererIncludeDirectories ${PNG_INCLUDE_DIRS})
	list(APPEND SPRDGLES3RendererLibraries          ${PNG_LIBRARIES})

	# Transparently use either bundled or system zlib.
	list(APPEND SPRDGLES3RendererIncludeDirectories ${ZLIB_INCLUDE_DIR})
	list(APPEND SPRDGLES3RendererLibraries          ${ZLIB_LIBRARIES})

	# Transparently use our bundled minizip.
	list(APPEND SPRDGLES3RendererIncludeDirectories ${MINIZIP_INCLUDE_DIRS})
	list(APPEND SPRDGLES3RendererLibraries          ${MINIZIP_LIBRARIES})

	# GLES3/EGL - AuroraOS target is GLES3-only; on desktop hosts pkg-config
	# provides the same libGLESv2/libEGL (Mesa).
	if (NOT WIN32)
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(GLESV2 REQUIRED glesv2)
		pkg_check_modules(EGL REQUIRED egl)
		list(APPEND SPRDGLES3RendererIncludeDirectories ${GLESV2_INCLUDE_DIRS} ${EGL_INCLUDE_DIRS})
		list(APPEND SPRDGLES3RendererLibraries ${GLESV2_LINK_LIBRARIES} ${EGL_LINK_LIBRARIES})
	endif()

	# Transparantly make use of all renderer directories
	list(APPEND SPRDGLES3RendererIncludeDirectories "${SPDir}/rd-common")

	list(APPEND SPRDGLES3RendererIncludeDirectories ${OpenJKLibDir})

	add_library(${SPRDGLES3Renderer} SHARED ${SPRDGLES3Files})

	if(NOT MSVC)
		# remove "lib" prefix for .so/.dylib files
		set_target_properties(${SPRDGLES3Renderer} PROPERTIES PREFIX "")
	endif()

	if(WIN32)
		install(TARGETS ${SPRDGLES3Renderer}
			RUNTIME
			DESTINATION ${JKAInstallDir}
			COMPONENT ${JKASPClientComponent})
	else(WIN32)
		if(MakeApplicationBundles AND (BuildSPEngine OR BuildJK2SPEngine))
			install(TARGETS ${SPRDGLES3Renderer}
				LIBRARY
				DESTINATION "${JKAInstallDir}/${SPEngine}.app/Contents/MacOS/"
				COMPONENT ${JKASPClientComponent})
		else()
			install(TARGETS ${SPRDGLES3Renderer}
				LIBRARY
				DESTINATION ${JKAInstallDir}
				COMPONENT ${JKASPClientComponent})
		endif()
	endif()

	set_target_properties(${SPRDGLES3Renderer} PROPERTIES COMPILE_DEFINITIONS "${SPRDGLES3Defines}")

	# Hide symbols not explicitly marked public.
	set_property(TARGET ${SPRDGLES3Renderer} APPEND PROPERTY COMPILE_OPTIONS ${OPENJK_VISIBILITY_FLAGS})

	set_target_properties(${SPRDGLES3Renderer} PROPERTIES INCLUDE_DIRECTORIES "${SPRDGLES3RendererIncludeDirectories}")
	set_target_properties(${SPRDGLES3Renderer} PROPERTIES PROJECT_LABEL "SP GLES3 Renderer")
	target_link_libraries(${SPRDGLES3Renderer} ${SPRDGLES3RendererLibraries})

endif(BuildSPRdGLES3)
