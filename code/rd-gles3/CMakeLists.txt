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
# along with this program; if not, see <http://www.gnu.org/licenses/>.
#============================================================================

# Make sure the user is not executing this script directly
if(NOT InOpenJK)
	message(FATAL_ERROR "Use the top-level cmake script!")
endif(NOT InOpenJK)

if(BuildSPRdGLES3)

	set(SPRDGLES3RendererIncludeDirectories ${SPDir})
	set(SPRDGLES3RendererIncludeDirectories
		${SharedDir}
		${SPRDGLES3RendererIncludeDirectories}
		"${SPDir}/rd-gles3"
		"${GSLIncludeDirectory}"
		)

	# Defines
	set(SPRDGLES3Defines ${SharedDefines} "RENDERER" "_JK2EXE" ${AuroraDefines})

	# Files

	# GHOUL 2
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

	# Transparantly make use of all renderer directories
	list(APPEND SPRDGLES3RendererIncludeDirectories "${SPDir}/rd-common")
	list(APPEND SPRDGLES3RendererIncludeDirectories "${SPDir}/rd-gles3")

	# Misc files
	set(SPRDGLES3CommonFiles
		"${SPDir}/qcommon/matcomp.cpp"
		"${SPDir}/qcommon/q_shared.cpp"
		"${SPDir}/qcommon/q_shared.h"
		"${SPDir}/qcommon/ojk_i_saved_game.h"
		"${SPDir}/qcommon/ojk_saved_game_class_archivers.h"
		"${SPDir}/qcommon/ojk_saved_game_helper.h"
		"${SPDir}/qcommon/ojk_saved_game_helper_fwd.h"
		${SharedCommonFiles}
		)
	source_group("common" FILES ${SPRDGLES3CommonFiles})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3CommonFiles})

	# Misc files
	set(SPRDGLES3CommonSafeFiles
		${SharedCommonSafeFiles}
		)
	source_group("common/safe" FILES ${SPRDGLES3CommonSafeFiles})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3CommonSafeFiles})

	# main files
	set(SPRDGLES3MainFiles
		"${SPDir}/rd-gles3/qgl.h"
		"${SPDir}/rd-gles3/tr_backend.cpp"
		"${SPDir}/rd-gles3/tr_bsp.cpp"
		"${SPDir}/rd-gles3/tr_cmds.cpp"
		"${SPDir}/rd-gles3/tr_curve.cpp"
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
		)
	source_group("renderer" FILES ${SPRDGLES3MainFiles})
	set(SPRDGLES3Files ${SPRDGLES3Files} ${SPRDGLES3MainFiles})

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

	# AuroraOS has no desktop OpenGL; the GLES3 renderer links GLESv2/EGL only.
	# pkg-config lookup is unconditional so the module can also be smoke-built
	# on a desktop host with mesa/GLES headers (BuildSPRdGLES3 defaults to OFF
	# unless AURORA is enabled).
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(GLESV2 REQUIRED glesv2)
	pkg_check_modules(EGL REQUIRED egl)
	set(SPRDGLES3RendererIncludeDirectories ${SPRDGLES3RendererIncludeDirectories} ${GLESV2_INCLUDE_DIRS} ${EGL_INCLUDE_DIRS})
	set(SPRDGLES3RendererLibraries ${SPRDGLES3RendererLibraries} ${GLESV2_LINK_LIBRARIES} ${EGL_LINK_LIBRARIES})

	set(SPRDGLES3RendererIncludeDirectories ${SPRDGLES3RendererIncludeDirectories} ${OpenJKLibDir})

	function(add_sp_gles3_renderer_project ProjectName Label EngineName InstallDir Component)
		add_library(${ProjectName} SHARED ${SPRDGLES3Files})
		if(NOT MSVC)
			# remove "lib" prefix for .so/.dylib files
			set_target_properties(${ProjectName} PROPERTIES PREFIX "")
		endif()

		if(WIN32)
			install(TARGETS ${ProjectName}
				RUNTIME
				DESTINATION ${InstallDir}
				COMPONENT ${Component})
		else(WIN32)
			if(MakeApplicationBundles AND (BuildSPEngine OR BuildJK2SPEngine))
				install(TARGETS ${ProjectName}
					LIBRARY
					DESTINATION "${InstallDir}/${EngineName}.app/Contents/MacOS/"
					COMPONENT ${Component})
			else()
				install(TARGETS ${ProjectName}
					LIBRARY
					DESTINATION ${InstallDir}
					COMPONENT ${Component})
			endif()
		endif()

		set_target_properties(${ProjectName} PROPERTIES COMPILE_DEFINITIONS "${SPRDGLES3Defines}")

		# Hide symbols not explicitly marked public.
		set_property(TARGET ${ProjectName} APPEND PROPERTY COMPILE_OPTIONS ${OPENJK_VISIBILITY_FLAGS})

		set_target_properties(${ProjectName} PROPERTIES INCLUDE_DIRECTORIES "${SPRDGLES3RendererIncludeDirectories}")
		set_target_properties(${ProjectName} PROPERTIES PROJECT_LABEL ${Label})

		target_link_libraries(${ProjectName} ${SPRDGLES3RendererLibraries})
	endfunction(add_sp_gles3_renderer_project)

	add_sp_gles3_renderer_project(${SPRDGLES3Renderer} "SP GLES3 Renderer" ${SPEngine} ${JKAInstallDir} ${JKASPClientComponent})

endif(BuildSPRdGLES3)
