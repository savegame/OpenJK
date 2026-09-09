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

if(BuildSPRdVulkan)

	set(SPRDVulkanRendererIncludeDirectories
		${SharedDir}
		${SPDir}
		"${SPDir}/rd-vulkan"
		"${SPDir}/rd-vulkan/vulkan"
		"${GSLIncludeDirectory}"
		)

	# Defines
	set(SPRDVulkanDefines ${SharedDefines} "RENDERER" "_JK2EXE" ${AuroraDefines})

	# GHOUL 2 (SP versions, shared with rd-vanilla/rd-gles3 semantics)
	set(SPRDVulkanG2Files
		"${SPDir}/ghoul2/G2.h"
		"${SPDir}/ghoul2/ghoul2_gore.h"
		"${SPDir}/rd-vulkan/G2_API.cpp"
		"${SPDir}/rd-vulkan/G2_bolts.cpp"
		"${SPDir}/rd-vulkan/G2_bones.cpp"
		"${SPDir}/rd-vulkan/G2_misc.cpp"
		"${SPDir}/rd-vulkan/G2_surfaces.cpp"
		)
	source_group("ghoul2" FILES ${SPRDVulkanG2Files})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanG2Files})

	# main renderer files (ported from codemp/rd-vulkan)
	set(SPRDVulkanMainFiles
		"${SPDir}/rd-vulkan/tr_backend.cpp"
		"${SPDir}/rd-vulkan/tr_bsp.cpp"
		"${SPDir}/rd-vulkan/tr_cache.cpp"
		"${SPDir}/rd-vulkan/tr_cache.h"
		"${SPDir}/rd-vulkan/tr_cmds.cpp"
		"${SPDir}/rd-vulkan/tr_curve.cpp"
		"${SPDir}/rd-vulkan/tr_decals.cpp"
		"${SPDir}/rd-vulkan/tr_draw.cpp"
		"${SPDir}/rd-vulkan/tr_ghoul2.cpp"
		"${SPDir}/rd-vulkan/tr_image.cpp"
		"${SPDir}/rd-vulkan/tr_init.cpp"
		"${SPDir}/rd-vulkan/tr_light.cpp"
		"${SPDir}/rd-vulkan/tr_local.h"
		"${SPDir}/rd-vulkan/tr_main.cpp"
		"${SPDir}/rd-vulkan/tr_marks.cpp"
		"${SPDir}/rd-vulkan/tr_mesh.cpp"
		"${SPDir}/rd-vulkan/tr_model.cpp"
		"${SPDir}/rd-vulkan/tr_quicksprite.cpp"
		"${SPDir}/rd-vulkan/tr_quicksprite.h"
		"${SPDir}/rd-vulkan/tr_scene.cpp"
		"${SPDir}/rd-vulkan/tr_shade.cpp"
		"${SPDir}/rd-vulkan/tr_shade_calc.cpp"
		"${SPDir}/rd-vulkan/tr_shader.cpp"
		"${SPDir}/rd-vulkan/tr_shadows.cpp"
		"${SPDir}/rd-vulkan/tr_skin.cpp"
		"${SPDir}/rd-vulkan/tr_sky.cpp"
		"${SPDir}/rd-vulkan/tr_stl.cpp"
		"${SPDir}/rd-vulkan/tr_stl.h"
		"${SPDir}/rd-vulkan/tr_subs.cpp"
		"${SPDir}/rd-vulkan/tr_surface.cpp"
		"${SPDir}/rd-vulkan/tr_surfacesprites.cpp"
		"${SPDir}/rd-vulkan/tr_world.cpp"
		"${SPDir}/rd-vulkan/tr_WorldEffects.cpp"
		"${SPDir}/rd-vulkan/tr_WorldEffects.h"
		"${SPDir}/rd-vulkan/vk_attachments.cpp"
		"${SPDir}/rd-vulkan/vk_bloom.cpp"
		"${SPDir}/rd-vulkan/vk_cmd.cpp"
		"${SPDir}/rd-vulkan/vk_debug.cpp"
		"${SPDir}/rd-vulkan/vk_dynamic_glow.cpp"
		"${SPDir}/rd-vulkan/vk_flares.cpp"
		"${SPDir}/rd-vulkan/vk_frame.cpp"
		"${SPDir}/rd-vulkan/vk_image.cpp"
		"${SPDir}/rd-vulkan/vk_image_process.cpp"
		"${SPDir}/rd-vulkan/vk_info.cpp"
		"${SPDir}/rd-vulkan/vk_init.cpp"
		"${SPDir}/rd-vulkan/vk_instance.cpp"
		"${SPDir}/rd-vulkan/vk_local.h"
		"${SPDir}/rd-vulkan/vk_pipelines.cpp"
		"${SPDir}/rd-vulkan/vk_shaders.cpp"
		"${SPDir}/rd-vulkan/vk_shade_geometry.cpp"
		"${SPDir}/rd-vulkan/vk_swapchain.cpp"
		"${SPDir}/rd-vulkan/vk_vbo.cpp"
		"${SPDir}/rd-vulkan/vk_vbo_surfacesprites.cpp"
		)
	source_group("renderer" FILES ${SPRDVulkanMainFiles})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanMainFiles})

	# vulkan headers (bundled)
	set(SPRDVulkanVulkanAssets
		"${SPDir}/rd-vulkan/vulkan/vk_platform.h"
		"${SPDir}/rd-vulkan/vulkan/vulkan.h"
		"${SPDir}/rd-vulkan/vulkan/vulkan_core.h"
		"${SPDir}/rd-vulkan/vulkan/vulkan_win32.h"
		"${SPDir}/rd-vulkan/vulkan/vulkan_xlib.h"
		"${SPDir}/rd-vulkan/vulkan/vulkan_xlib_xrandr.h"
		)
	source_group("renderer\\vulkan" FILES ${SPRDVulkanVulkanAssets})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanVulkanAssets})

	set(SPRDVulkanVulkanVideoAssets
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codecs_common.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_av1std.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_av1std_decode.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_av1std_encode.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_h264std.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_h264std_decode.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_h264std_encode.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_h265std.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_h265std_decode.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_h265std_encode.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_vp9std.h"
		"${SPDir}/rd-vulkan/vulkan/vk_video/vulkan_video_codec_vp9std_decode.h"
		)
	source_group("renderer\\vulkan\\vk_video" FILES ${SPRDVulkanVulkanVideoAssets})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanVulkanVideoAssets})

	set(SPRDVulkanUtils
		"${SPDir}/rd-vulkan/utils/stb_dxt.h")
	source_group("renderer\\utils" FILES ${SPDVulkanUtils})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPDVulkanUtils})

	set(SPRDVulkanRdCommonFiles
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
	source_group("rd-common" FILES ${SPRDVulkanRdCommonFiles})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanRdCommonFiles})

	set(SPRDVulkanCommonFiles
		"${SPDir}/qcommon/matcomp.cpp"
		"${SPDir}/qcommon/q_shared.cpp"

		${SharedCommonFiles})
	source_group("common" FILES ${SPRDVulkanCommonFiles})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanCommonFiles})

	set(SPRDVulkanCommonSafeFiles
		${SharedCommonSafeFiles}
		)
	source_group("common/safe" FILES ${SPRDVulkanCommonSafeFiles})
	set(SPRDVulkanFiles ${SPRDVulkanFiles} ${SPRDVulkanCommonSafeFiles})

	# Transparently use either bundled or system jpeg.
	list(APPEND SPRDVulkanRendererIncludeDirectories ${JPEG_INCLUDE_DIR})
	list(APPEND SPRDVulkanRendererLibraries          ${JPEG_LIBRARIES})

	# Transparently use either bundled or system libpng.  Order is important --
	# libpng used zlib, so it must come before it on the linker command line.
	list(APPEND SPRDVulkanRendererIncludeDirectories ${PNG_INCLUDE_DIRS})
	list(APPEND SPRDVulkanRendererLibraries          ${PNG_LIBRARIES})

	# Transparently use either bundled or system zlib.
	list(APPEND SPRDVulkanRendererIncludeDirectories ${ZLIB_INCLUDE_DIR})
	list(APPEND SPRDVulkanRendererLibraries          ${ZLIB_LIBRARIES})

	# Transparently use our bundled minizip.
	list(APPEND SPRDVulkanRendererIncludeDirectories ${MINIZIP_INCLUDE_DIRS})
	list(APPEND SPRDVulkanRendererLibraries          ${MINIZIP_LIBRARIES})

	# Transparantly make use of all renderer directories
	list(APPEND SPRDVulkanRendererIncludeDirectories "${SPDir}/rd-common")

	list(APPEND SPRDVulkanRendererIncludeDirectories ${OpenJKLibDir})

	add_library(${SPRDVulkanRenderer} SHARED ${SPRDVulkanFiles})

	if(NOT MSVC)
		# remove "lib" prefix for .so/.dylib files
		set_target_properties(${SPRDVulkanRenderer} PROPERTIES PREFIX "")
	endif()

	if(WIN32)
		install(TARGETS ${SPRDVulkanRenderer}
			RUNTIME
			DESTINATION ${JKAInstallDir}
			COMPONENT ${JKASPClientComponent})
	else(WIN32)
		if(MakeApplicationBundles AND (BuildSPEngine OR BuildJK2SPEngine))
			install(TARGETS ${SPRDVulkanRenderer}
				LIBRARY
				DESTINATION "${JKAInstallDir}/${SPEngine}.app/Contents/MacOS/"
				COMPONENT ${JKASPClientComponent})
		else()
			install(TARGETS ${SPRDVulkanRenderer}
				LIBRARY
				DESTINATION ${JKAInstallDir}
				COMPONENT ${JKASPClientComponent})
		endif()
	endif()

	set_target_properties(${SPRDVulkanRenderer} PROPERTIES COMPILE_DEFINITIONS "${SPRDVulkanDefines}")

	# Hide symbols not explicitly marked public.
	set_property(TARGET ${SPRDVulkanRenderer} APPEND PROPERTY COMPILE_OPTIONS ${OPENJK_VISIBILITY_FLAGS})

	set_target_properties(${SPRDVulkanRenderer} PROPERTIES INCLUDE_DIRECTORIES "${SPRDVulkanRendererIncludeDirectories}")
	set_target_properties(${SPRDVulkanRenderer} PROPERTIES PROJECT_LABEL "SP Vulkan Renderer")
	target_link_libraries(${SPRDVulkanRenderer} ${SPRDVulkanRendererLibraries})

endif(BuildSPRdVulkan)
