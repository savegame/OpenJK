/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
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

// tr_shade.c

#include "../server/exe_headers.h"

#include "tr_local.h"
#include "tr_gles3.h"

/*

  THIS ENTIRE FILE IS BACK END

  This file deals with applying shaders to surface data in the tess struct.
*/

shaderCommands_t	tess;

color4ub_t	styleColors[MAX_LIGHT_STYLES];
bool		styleUpdated[MAX_LIGHT_STYLES];

extern bool g_bRenderGlowingObjects;

void R_BindAnimatedImage( const textureBundle_t *bundle );

/*
** RB_EmitStage
**
** Draw the current tess batch for one shader stage through the GLES3
** streaming path: pipeline definition (state bits + cull + offset) ->
** cached GLSL program, CPU-computed MVP, streamed vertex data.
** TODO(M3): multitexture/lightmap bundle[1] combination, texture-env modes.
** TODO(M4): fog pass / fog collapse into the stage pipeline.
*/
static void RB_EmitStage( shaderStage_t *pStage )
{
	g3_pipeline_def_t def;

	memset( &def, 0, sizeof( def ) );
	def.type = G3_PROG_TEXTURE;
	def.state_bits = pStage->stateBits;
	def.cull_type = backEnd.projection2D ? CT_TWO_SIDED : tess.shader->cullType;
	def.mirror = backEnd.viewParms.isMirror ? true : false;
	def.polygon_offset = backEnd.projection2D ? false : tess.shader->polygonOffset;

	GL_SelectTexture( 0 );
	if ( pStage->bundle[0].image != NULL ||
		 pStage->bundle[0].numImageAnimations > 0 ||
		 pStage->bundle[0].isVideoMap ) {
		R_BindAnimatedImage( &pStage->bundle[0] );
	} else {
		// stage without a texture (e.g. pure rgbGen color): bind white so a
		// single textured program covers both cases
		GL_Bind( tr.whiteImage );
	}

	g3_draw_tess( &def, tess.numIndexes, tess.indexes );
}

/*
================
R_ArrayElementDiscrete

This is just for OpenGL conformance testing, it should never be the fastest
================
*/
/*
TODO(M3): R_ArrayElementDiscrete / R_DrawStripElements (conformance-test
primitive paths) relied on immediate mode and are dropped; tess output goes
through the streaming buffer (RB_EmitStage).
*/





/*
=============================================================

SURFACE SHADERS

=============================================================
*/


/*
=================
R_BindAnimatedImage

=================
*/
void R_BindAnimatedImage( const textureBundle_t *bundle) {
	int		index;

	if ( bundle->isVideoMap ) {
		ri.CIN_RunCinematic(bundle->videoMapHandle);
		ri.CIN_UploadCinematic(bundle->videoMapHandle);
		return;
	}

	if ((r_fullbright->integer || tr.refdef.doLAGoggles || (tr.refdef.rdflags & RDF_doFullbright) ) && bundle->isLightmap)
	{
		GL_Bind( tr.whiteImage );
		return;
	}

	if ( bundle->numImageAnimations <= 1 ) {
		GL_Bind( bundle->image );
		return;
	}

	if (backEnd.currentEntity->e.renderfx & RF_SETANIMINDEX )
	{
		index = backEnd.currentEntity->e.skinNum;
	}
	else
	{
		// it is necessary to do this messy calc to make sure animations line up
		// exactly with waveforms of the same frequency
		index = Q_ftol( backEnd.refdef.floatTime * bundle->imageAnimationSpeed * FUNCTABLE_SIZE );
		index >>= FUNCTABLE_SIZE2;

		if ( index < 0 ) {
			index = 0;	// may happen with shader time offsets
		}
	}

	if ( bundle->oneShotAnimMap )
	{
		if ( index >= bundle->numImageAnimations )
		{
			// stick on last frame
			index = bundle->numImageAnimations - 1;
		}
	}
	else
	{
		// loop
		index %= bundle->numImageAnimations;
	}

	GL_Bind( *((image_t**)bundle->image + index) );
}


/*
================
DrawTris

Draws triangle outlines for debugging
================
*/
static void DrawTris (shaderCommands_t *input)
{
	// TODO(M3): r_showtris overlay used immediate mode / client arrays;
	// reimplement with a wireframe debug pipeline (GLSL) later.
	(void)input;
}

/*
================
DrawNormals

Draws vertex normals for debugging
================
*/
static void DrawNormals (shaderCommands_t *input) {
	// TODO(M3): r_shownormals overlay used immediate mode; debug pipeline.
	(void)input;
}


/*
==============
RB_BeginSurface

We must set some things up before beginning any tesselation,
because a surface may be forced to perform a RB_End due
to overflow.
==============
*/
void RB_BeginSurface( shader_t *shader, int fogNum ) {
//	shader_t *state = (shader->remappedShader) ? shader->remappedShader : shader;
	shader_t *state = shader;

	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.shader = state;//shader;
	tess.fogNum = fogNum;
	tess.dlightBits = 0;		// will be OR'd in by surface functions

	tess.SSInitializedWind = qfalse;	//is this right?

	tess.xstages = state->stages;
	tess.numPasses = state->numUnfoggedPasses;

	tess.currentStageIteratorFunc = shader->sky ? RB_StageIteratorSky : RB_StageIteratorGeneric;

	tess.fading = false;

	tess.registration++;
}

/*
===================
DrawMultitextured

output = t0 * t1 or t0 + t1

t0 = most upstream according to spec
t1 = most downstream according to spec
===================
*/
static void DrawMultitextured( shaderCommands_t *input, int stage ) {
	// TODO(M3): multitexture collapse (lightmap on unit 1, texture env)
	// becomes a multi-texture pipeline def bound from the streaming path
	// (plan section 1.1).
	(void)input; (void)stage;
}

//--EF_old dlight code...reverting back to Quake III dlight to see if people like that better
// Lifted the whole function because someone hacked the heck out of this and it doesn't seem to
//	be a case where it's as easy as just changing the blend mode....
/*
===================
ProjectDlightTexture

Perform dynamic lighting with another rendering pass
===================
*/
/*
static void ProjectDlightTexture( void ) {
	int		l;
	vec3_t	origin;
	float	*texCoords;
	byte	*colors;
	byte	clipBits[SHADER_MAX_VERTEXES];
	float	texCoordsArray[SHADER_MAX_VERTEXES][2];
	byte	colorArray[SHADER_MAX_VERTEXES][4];
	unsigned	hitIndexes[SHADER_MAX_INDEXES];

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		int		numIndexes;
		vec3_t	floatColor;
		float	scale;
		float	radius, chord;
		dlight_t	*dl;
		int i;

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;	// this surface definately doesn't have any of this light
		}
		texCoords = texCoordsArray[0];
		colors = colorArray[0];

		dl = &backEnd.refdef.dlights[l];
		VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		chord = radius*radius*0.25f;
		scale = 1.0f / radius;
		floatColor[0] = dl->color[0] * 255f;
		floatColor[1] = dl->color[1] * 255f;
		floatColor[2] = dl->color[2] * 255f;

		for ( i = 0 ; i < tess.numVertexes ; i++, texCoords += 2, colors += 4 ) {
			vec3_t	distVec;
			int		clip;
			float	tempColor;
			float	modulate, dist;

//			if ( 0 ) {
//				clipBits[i] = 255;	// definately not dlighted
//				continue;
//			}
//
			backEnd.pc.c_dlightVertexes++;

			VectorSubtract( origin, tess.xyz[i], distVec );
			dist = VectorLengthSquared(distVec);

			texCoords[0] = 0.5 + distVec[0] * scale;	//xy projection
			texCoords[1] = 0.5 + distVec[1] * scale;

			clip = 0;
			if ( texCoords[0] < 0 ) {
				clip |= 1;
			} else if ( texCoords[0] > 1 ) {
				clip |= 2;
			}
			if ( texCoords[1] < 0 ) {
				clip |= 4;
			} else if ( texCoords[1] > 1 ) {
				clip |= 8;
			}
			clipBits[i] = clip;

			// modulate the strength based on the height and color
			if ( dist > chord) {
				clip |= 16;
				modulate = 255*1.0ff;
			} else {
				modulate = 255*2*dist*scale*scale;
			}
			tempColor = floatColor[0] + modulate;
			colors[0] = tempColor > 255 ? 255: Q_ftol(tempColor);

			tempColor = floatColor[1] + modulate;
			colors[1] = tempColor > 255 ? 255: Q_ftol(tempColor);

			tempColor = floatColor[2] + modulate;
			colors[2] = tempColor > 255 ? 255: Q_ftol(tempColor);

//			colors[3] = 255;
			if ( distVec[2] > radius ) {
				colors[3] = 0;
			} else if ( distVec[2] < -radius ) {
				colors[3] = 0;
			} else {
				if ( distVec[2] < 0 ) {
					distVec[2] = -distVec[2];
				}
				if ( distVec[2] < radius * 0.5 ) {
					colors[3] = 255;
				} else {
					colors[3] = Q_ftol(255* (radius - distVec[2]) * scale);
				}
			}

		}

		// build a list of triangles that need light
		numIndexes = 0;
		for ( i = 0 ; i < tess.numIndexes ; i += 3 ) {
			int		a, b, c;

			a = tess.indexes[i];
			b = tess.indexes[i+1];
			c = tess.indexes[i+2];
			if ( clipBits[a] & clipBits[b] & clipBits[c] ) {
				continue;	// not lighted
			}
			hitIndexes[numIndexes] = a;
			hitIndexes[numIndexes+1] = b;
			hitIndexes[numIndexes+2] = c;
			numIndexes += 3;
		}

		if ( !numIndexes ) {
			continue;
		}

		qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
		qglTexCoordPointer( 2, GL_FLOAT, 0, texCoordsArray[0] );

		qglEnableClientState( GL_COLOR_ARRAY );
		qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, colorArray );

		GL_Bind( tr.dlightImage );

		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_SRC_COLOR | GLS_DEPTHFUNC_EQUAL);//our way
//		GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );	//Id way
		R_DrawElements( numIndexes, hitIndexes );
		backEnd.pc.c_totalIndexes += numIndexes;
		backEnd.pc.c_dlightIndexes += numIndexes;
	}
}
*/

// Lifted from Quake III to see if people like this kind of dlight better
/*
===================
ProjectDlightTexture

Perform dynamic lighting with another rendering pass
===================
*/
static void ProjectDlightTexture2( void ) {
	// TODO(M6): dynamic light projection pass (clipped dlight texture +
	// attenuation colors) becomes a dlight shader pass (plan section M6).
}
static void ProjectDlightTexture( void ) {
	// TODO(M6): dynamic light projection pass (Q3-style dlight texture);
	// see ProjectDlightTexture2 above.
}

/*
===================
RB_FogPass

Blends a fog texture on top of everything else
===================
*/
static void RB_FogPass( void ) {
	// TODO(M4): the fog texture pass (fog-colored vertex colors + fog
	// texture texcoords) moves to the fog pipeline of the shader backend
	// (plan section 1.5, fog.vert/fog.frag in gles3-quake3e).
}


/*
===============
ComputeColors
===============
*/
static void ComputeColors( shaderStage_t *pStage, alphaGen_t forceAlphaGen, colorGen_t forceRGBGen )
{
	int i;

	if ( tess.shader != tr.projectionShadowShader && tess.shader != tr.shadowShader &&
			( backEnd.currentEntity->e.renderfx & (RF_DISINTEGRATE1|RF_DISINTEGRATE2)))
	{
		RB_CalcDisintegrateColors( (unsigned char *)tess.svars.colors, pStage->rgbGen );
		RB_CalcDisintegrateVertDeform();

		// We've done some custom alpha and color stuff, so we can skip the rest.  Let it do fog though
		forceRGBGen = CGEN_SKIP;
		forceAlphaGen = AGEN_SKIP;
	}

	//
	// rgbGen
	//
	if ( !forceRGBGen )
	{
		forceRGBGen = pStage->rgbGen;
	}

	if ( backEnd.currentEntity->e.renderfx & RF_VOLUMETRIC ) // does not work for rotated models, technically, this should also be a CGEN type, but that would entail adding new shader commands....which is too much work for one thing
	{
		int			i;
		float		*normal, dot;
		unsigned char *color;
		int			numVertexes;

		normal = tess.normal[0];
		color = tess.svars.colors[0];

		numVertexes = tess.numVertexes;

		for ( i = 0 ; i < numVertexes ; i++, normal += 4, color += 4)
		{
			dot = DotProduct( normal, backEnd.refdef.viewaxis[0] );

			dot *= dot * dot * dot;

			if ( dot < 0.2f ) // so low, so just clamp it
			{
				dot = 0.0f;
			}

			color[0] = color[1] = color[2] = color[3] = Q_ftol( backEnd.currentEntity->e.shaderRGBA[0] * (1-dot) );
		}

		forceRGBGen = CGEN_SKIP;
		forceAlphaGen = AGEN_SKIP;
	}

	if ( !forceAlphaGen )	//set this up so we can override below
	{
		forceAlphaGen = pStage->alphaGen;
	}

	switch ( forceRGBGen )
	{
		case CGEN_SKIP:
			break;
		case CGEN_IDENTITY:
			memset( tess.svars.colors, 0xff, tess.numVertexes * 4 );
			break;
		default:
		case CGEN_IDENTITY_LIGHTING:
			memset( tess.svars.colors, tr.identityLightByte, tess.numVertexes * 4 );
			break;
		case CGEN_LIGHTING_DIFFUSE:
			RB_CalcDiffuseColor( ( unsigned char * ) tess.svars.colors );
			break;
		case CGEN_LIGHTING_DIFFUSE_ENTITY:
			RB_CalcDiffuseEntityColor( ( unsigned char * ) tess.svars.colors );

			if ( forceAlphaGen == AGEN_IDENTITY &&
				 backEnd.currentEntity->e.shaderRGBA[3] == 0xff
				)
			{
				forceAlphaGen = AGEN_SKIP;	//already got it in this set since it does all 4 components
			}
			break;
		case CGEN_EXACT_VERTEX:
			memcpy( tess.svars.colors, tess.vertexColors, tess.numVertexes * sizeof( tess.vertexColors[0] ) );
			break;
		case CGEN_CONST:
			for ( i = 0; i < tess.numVertexes; i++ ) {
				byteAlias_t *baDest = (byteAlias_t *)&tess.svars.colors[i],
					*baSource = (byteAlias_t *)&pStage->constantColor;
				baDest->i = baSource->i;
			}
			break;
		case CGEN_VERTEX:
			if ( tr.identityLight == 1 )
			{
				memcpy( tess.svars.colors, tess.vertexColors, tess.numVertexes * sizeof( tess.vertexColors[0] ) );
			}
			else
			{
				for ( i = 0; i < tess.numVertexes; i++ )
				{
					tess.svars.colors[i][0] = tess.vertexColors[i][0] * tr.identityLight;
					tess.svars.colors[i][1] = tess.vertexColors[i][1] * tr.identityLight;
					tess.svars.colors[i][2] = tess.vertexColors[i][2] * tr.identityLight;
					tess.svars.colors[i][3] = tess.vertexColors[i][3];
				}
			}
			break;
		case CGEN_ONE_MINUS_VERTEX:
			if ( tr.identityLight == 1 )
			{
				for ( i = 0; i < tess.numVertexes; i++ )
				{
					tess.svars.colors[i][0] = 255 - tess.vertexColors[i][0];
					tess.svars.colors[i][1] = 255 - tess.vertexColors[i][1];
					tess.svars.colors[i][2] = 255 - tess.vertexColors[i][2];
				}
			}
			else
			{
				for ( i = 0; i < tess.numVertexes; i++ )
				{
					tess.svars.colors[i][0] = ( 255 - tess.vertexColors[i][0] ) * tr.identityLight;
					tess.svars.colors[i][1] = ( 255 - tess.vertexColors[i][1] ) * tr.identityLight;
					tess.svars.colors[i][2] = ( 255 - tess.vertexColors[i][2] ) * tr.identityLight;
				}
			}
			break;
		case CGEN_FOG:
			{
				const fog_t *fog = tr.world->fogs + tess.fogNum;

				for ( i = 0; i < tess.numVertexes; i++ ) {
					byteAlias_t *ba = (byteAlias_t *)&tess.svars.colors[i];
					ba->i = fog->colorInt;
				}
			}
			break;
		case CGEN_WAVEFORM:
			RB_CalcWaveColor( &pStage->rgbWave, ( unsigned char * ) tess.svars.colors );
			break;
		case CGEN_ENTITY:
			RB_CalcColorFromEntity( ( unsigned char * ) tess.svars.colors );
			if ( forceAlphaGen == AGEN_IDENTITY &&
				 backEnd.currentEntity->e.shaderRGBA[3] == 0xff
				)
			{
				forceAlphaGen = AGEN_SKIP;	//already got it in this set since it does all 4 components
			}

			break;
		case CGEN_ONE_MINUS_ENTITY:
			RB_CalcColorFromOneMinusEntity( ( unsigned char * ) tess.svars.colors );
			break;
		case CGEN_LIGHTMAPSTYLE:
			for ( i = 0; i < tess.numVertexes; i++ )
			{
				byteAlias_t *baDest = (byteAlias_t *)&tess.svars.colors[i],
					*baSource = (byteAlias_t *)&styleColors[pStage->lightmapStyle];
				baDest->ui = baSource->ui;
			}
			break;
		}

	//
	// alphaGen
	//

	switch ( forceAlphaGen )
	{
	case AGEN_SKIP:
		break;
	case AGEN_IDENTITY:
		if ( forceRGBGen != CGEN_IDENTITY &&  forceRGBGen != CGEN_LIGHTING_DIFFUSE ) {
			if ( ( forceRGBGen == CGEN_VERTEX && tr.identityLight != 1 ) ||
				 forceRGBGen != CGEN_VERTEX ) {
				for ( i = 0; i < tess.numVertexes; i++ ) {
					tess.svars.colors[i][3] = 0xff;
				}
			}
		}
		break;
	case AGEN_CONST:
		if ( forceRGBGen != CGEN_CONST ) {
			for ( i = 0; i < tess.numVertexes; i++ ) {
				tess.svars.colors[i][3] = pStage->constantColor[3];
			}
		}
		break;
	case AGEN_WAVEFORM:
		RB_CalcWaveAlpha( &pStage->alphaWave, ( unsigned char * ) tess.svars.colors );
		break;
	case AGEN_LIGHTING_SPECULAR:
		RB_CalcSpecularAlpha( ( unsigned char * ) tess.svars.colors );
		break;
	case AGEN_ENTITY:
		if ( forceRGBGen != CGEN_ENTITY ) { //already got it in the CGEN_entity since it does all 4 components
			RB_CalcAlphaFromEntity( ( unsigned char * ) tess.svars.colors );
		}
		break;
	case AGEN_ONE_MINUS_ENTITY:
		RB_CalcAlphaFromOneMinusEntity( ( unsigned char * ) tess.svars.colors );
		break;
	case AGEN_VERTEX:
		if ( forceRGBGen != CGEN_VERTEX ) {
			for ( i = 0; i < tess.numVertexes; i++ ) {
				tess.svars.colors[i][3] = tess.vertexColors[i][3];
			}
		}
        break;
    case AGEN_ONE_MINUS_VERTEX:
		for ( i = 0; i < tess.numVertexes; i++ )
		{
			tess.svars.colors[i][3] = 255 - tess.vertexColors[i][3];
		}
		break;
	case AGEN_PORTAL:
		{
			unsigned char alpha;

			for ( i = 0; i < tess.numVertexes; i++ )
			{
				float len;
				vec3_t v;

				VectorSubtract( tess.xyz[i], backEnd.viewParms.ori.origin, v );
				len = VectorLength( v );

				len /= tess.shader->portalRange;

				if ( len < 0 )
				{
					alpha = 0;
				}
				else if ( len > 1 )
				{
					alpha = 0xff;
				}
				else
				{
					alpha = len * 0xff;
				}

				tess.svars.colors[i][3] = alpha;
			}
		}
		break;
	case AGEN_BLEND:
		if ( forceRGBGen != CGEN_VERTEX )
		{
			for ( i = 0; i < tess.numVertexes; i++ )
			{
				tess.svars.colors[i][3] = tess.vertexAlphas[i][pStage->index];
			}
		}
		break;
	default:
		break;
	}

	//
	// fog adjustment for colors to fade out as fog increases
	//
	if ( tess.fogNum )
	{
		switch ( pStage->adjustColorsForFog )
		{
		case ACFF_MODULATE_RGB:
			RB_CalcModulateColorsByFog( ( unsigned char * ) tess.svars.colors );
			break;
		case ACFF_MODULATE_ALPHA:
			RB_CalcModulateAlphasByFog( ( unsigned char * ) tess.svars.colors );
			break;
		case ACFF_MODULATE_RGBA:
			RB_CalcModulateRGBAsByFog( ( unsigned char * ) tess.svars.colors );
			break;
		case ACFF_NONE:
			break;
		default:
			break;
		}
	}
}

/*
===============
ComputeTexCoords
===============
*/
static void ComputeTexCoords( shaderStage_t *pStage ) {
	int		i;
	int b;

	for ( b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
		int tm;

		//
		// generate the texture coordinates
		//
		switch ( pStage->bundle[b].tcGen )
		{
		case TCGEN_IDENTITY:
			memset( tess.svars.texcoords[b], 0, sizeof( float ) * 2 * tess.numVertexes );
			break;
		case TCGEN_TEXTURE:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = tess.texCoords[i][0][0];
				tess.svars.texcoords[b][i][1] = tess.texCoords[i][0][1];
			}
			break;
		case TCGEN_LIGHTMAP:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = tess.texCoords[i][1][0];
				tess.svars.texcoords[b][i][1] = tess.texCoords[i][1][1];
			}
			break;
		case TCGEN_LIGHTMAP1:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = tess.texCoords[i][2][0];
				tess.svars.texcoords[b][i][1] = tess.texCoords[i][2][1];
			}
			break;
		case TCGEN_LIGHTMAP2:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = tess.texCoords[i][3][0];
				tess.svars.texcoords[b][i][1] = tess.texCoords[i][3][1];
			}
			break;
		case TCGEN_LIGHTMAP3:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = tess.texCoords[i][4][0];
				tess.svars.texcoords[b][i][1] = tess.texCoords[i][4][1];
			}
			break;
		case TCGEN_VECTOR:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = DotProduct( tess.xyz[i], pStage->bundle[b].tcGenVectors[0] );
				tess.svars.texcoords[b][i][1] = DotProduct( tess.xyz[i], pStage->bundle[b].tcGenVectors[1] );
			}
			break;
		case TCGEN_FOG:
			RB_CalcFogTexCoords( ( float * ) tess.svars.texcoords[b] );
			break;
		case TCGEN_ENVIRONMENT_MAPPED:
			if ( r_environmentMapping->integer )
				RB_CalcEnvironmentTexCoords( (float *)tess.svars.texcoords[b] );
			else
				memset( tess.svars.texcoords[b], 0, sizeof( float ) * 2 * tess.numVertexes );
			break;
		case TCGEN_BAD:
			return;
		}

		//
		// alter texture coordinates
		//
		for ( tm = 0; tm < pStage->bundle[b].numTexMods ; tm++ ) {
			switch ( pStage->bundle[b].texMods[tm].type )
			{
			case TMOD_NONE:
				tm = TR_MAX_TEXMODS;		// break out of for loop
				break;

			case TMOD_TURBULENT:
				RB_CalcTurbulentTexCoords( &pStage->bundle[b].texMods[tm].wave,
						                 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_ENTITY_TRANSLATE:
				RB_CalcScrollTexCoords( backEnd.currentEntity->e.shaderTexCoord,
									 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_SCROLL:
				RB_CalcScrollTexCoords( pStage->bundle[b].texMods[tm].translate,	//union scroll into translate
										 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_SCALE:
				RB_CalcScaleTexCoords( pStage->bundle[b].texMods[tm].translate,		//union scroll into translate
									 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_STRETCH:
				RB_CalcStretchTexCoords( &pStage->bundle[b].texMods[tm].wave,
						               ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_TRANSFORM:
				RB_CalcTransformTexCoords( &pStage->bundle[b].texMods[tm],
						                 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_ROTATE:
				RB_CalcRotateTexCoords( pStage->bundle[b].texMods[tm].translate[0], //union rotateSpeed into translate[0]
										( float * ) tess.svars.texcoords[b] );
				break;

			default:
				Com_Error( ERR_DROP, "ERROR: unknown texmod '%d' in shader '%s'\n", pStage->bundle[b].texMods[tm].type, tess.shader->name );
				break;
			}
		}
	}
}

void ForceAlpha(unsigned char *dstColors, int TR_ForceEntAlpha)
{
	int	i;

	dstColors += 3;

	for ( i = 0; i < tess.numVertexes; i++, dstColors += 4 )
	{
		*dstColors = TR_ForceEntAlpha;
	}
}

/*
** RB_IterateStagesGeneric
*/
extern bool tr_stencilled; //tr_backend.cpp
static void RB_IterateStagesGeneric( shaderCommands_t *input )
{
	int stage;

	for ( stage = 0; stage < input->shader->numUnfoggedPasses; stage++ )
	{
		shaderStage_t *pStage = &tess.xstages[stage];
		if ( !pStage->active )
		{
			break;
		}

		// Reject this stage if it's not a glow stage but we are doing a glow pass.
		if ( g_bRenderGlowingObjects && !pStage->glow )
		{
			continue;
		}

		alphaGen_t	forceAlphaGen = (alphaGen_t)0;
		colorGen_t	forceRGBGen = (colorGen_t)0;

		if ( backEnd.currentEntity )
		{
			if ( backEnd.currentEntity->e.renderfx & RF_ALPHA_FADE )
			{
				if ( backEnd.currentEntity->e.shaderRGBA[3] < 255 )
				{
					forceAlphaGen = AGEN_ENTITY;
				}
			}

			if ( backEnd.currentEntity->e.renderfx & RF_RGB_TINT )
			{//want to use RGBGen from ent
				forceRGBGen = CGEN_ENTITY;
			}
		}

		if (pStage->ss && pStage->ss->surfaceSpriteType)
		{
			// We check for surfacesprites AFTER drawing everything else
			continue;
		}

		if (!input->fading)
		{ //this means ignore this, while we do a fade-out
			ComputeColors( pStage, forceAlphaGen, forceRGBGen );
		}
		ComputeTexCoords( pStage );

		// emit the stage through the GLES3 streaming path
		RB_EmitStage( pStage );
	}
}

/*
** RB_StageIteratorGeneric
*/
void RB_StageIteratorGeneric( void )
{
	shaderCommands_t *input;
	int stage;

	input = &tess;

	RB_DeformTessGeometry();

	//
	// log this call
	//
	if ( r_logFile->integer )
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment( va("--- RB_StageIteratorGeneric( %s ) ---\n", tess.shader->name) );
	}

	//
	// set face culling appropriately
	//
	GL_Cull( input->shader->cullType );

	//
	// call shader function
	//
	RB_IterateStagesGeneric( input );

	//
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE
		&& !(tess.shader->surfaceFlags & (SURF_NODLIGHT | SURF_SKY) ) ) {
		if (r_dlightStyle->integer>0)
		{
			ProjectDlightTexture2();
		}
		else
		{
			ProjectDlightTexture();
		}
	}

	//
	// now do fog
	//
#ifdef JK2_MODE
	if (tess.fogNum && tess.shader->fogPass && r_drawfog->value)
#else
	if (tr.world && (tess.fogNum != tr.world->globalFog || r_drawfog->value != 2) && r_drawfog->value && tess.fogNum && tess.shader->fogPass)
#endif
	{
		RB_FogPass();
	}

	// polygon offset (if any) was a per-draw pipeline property in
	// RB_EmitStage; nothing to reset here.

	// Now check for surfacesprites.
	if (r_surfaceSprites->integer)
	{
		for ( stage = 1; stage < tess.shader->numUnfoggedPasses; stage++ )
		{
			if (tess.xstages[stage].ss && tess.xstages[stage].ss->surfaceSpriteType)
			{	// Draw the surfacesprite
				RB_DrawSurfaceSprites( &tess.xstages[stage], input);
			}
		}
	}

}


/*
** RB_EndSurface
*/
void RB_EndSurface( void ) {
	shaderCommands_t *input;

	input = &tess;

	if (input->numIndexes == 0) {
		return;
	}

	if (input->indexes[SHADER_MAX_INDEXES-1] != 0) {
		Com_Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_INDEXES hit");
	}
	if (input->xyz[SHADER_MAX_VERTEXES-1][0] != 0) {
		Com_Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_VERTEXES hit");
	}

	if ( tess.shader == tr.shadowShader ) {
		RB_ShadowTessEnd();
		return;
	}

	// for debugging of sort order issues, stop rendering after a given sort value
	if ( r_debugSort->integer && r_debugSort->integer < tess.shader->sort ) {
		return;
	}

	if ( skyboxportal )
	{
		// world
		if(!(backEnd.refdef.rdflags & RDF_SKYBOXPORTAL))
		{
			if(tess.currentStageIteratorFunc == RB_StageIteratorSky)
			{	// don't process these tris at all
				return;
			}
		}
		// portal sky
		else
		{
			if(!drawskyboxportal)
			{
				if( !(tess.currentStageIteratorFunc == RB_StageIteratorSky))
				{	// /only/ process sky tris
					return;
				}
			}
		}
	}

	//
	// update performance counters
	//
	if (!backEnd.projection2D)
	{
		backEnd.pc.c_shaders++;
		backEnd.pc.c_vertexes += tess.numVertexes;
		backEnd.pc.c_indexes += tess.numIndexes;
		backEnd.pc.c_totalIndexes += tess.numIndexes * tess.numPasses;
#ifdef JK2_MODE
		if (tess.fogNum && tess.shader->fogPass && r_drawfog->value)
#else
		if (tess.fogNum && tess.shader->fogPass && r_drawfog->value == 1)
#endif
		{	// Fogging adds an additional pass
			backEnd.pc.c_totalIndexes += tess.numIndexes;
		}
	}

	//
	// call off to shader specific tess end function
	//
	tess.currentStageIteratorFunc();

	//
	// draw debugging stuff
	//
	if ( r_showtris->integer )
	{
		DrawTris (input);
	}

	if ( r_shownormals->integer ) {
		DrawNormals (input);
	}

	// clear shader so we can tell we don't have any unclosed surfaces
	tess.numIndexes = 0;

	GLimp_LogComment( "----------\n" );
}

