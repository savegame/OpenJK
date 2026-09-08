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

// tr_QuickSprite.cpp: implementation of the CQuickSpriteSystem class.
//
//////////////////////////////////////////////////////////////////////
#include "../server/exe_headers.h"
#include "tr_quicksprite.h"

extern void R_BindAnimatedImage( const textureBundle_t *bundle );


//////////////////////////////////////////////////////////////////////
// Singleton System
//////////////////////////////////////////////////////////////////////
CQuickSpriteSystem SQuickSprite;


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CQuickSpriteSystem::CQuickSpriteSystem() :
	mTexBundle(NULL),
	mGLStateBits(0),
	mFogIndex(-1),
	mUseFog(qfalse),
	mNextVert(0)
{
	int i;

	memset( mVerts, 0, sizeof( mVerts ) );
	memset( mFogTextureCoords, 0, sizeof( mFogTextureCoords ) );
	memset( mColors, 0, sizeof( mColors ) );

	for (i = 0; i < SHADER_MAX_VERTEXES; i += 4)
	{
		// Bottom right
		mTextureCoords[i + 0][0] = 1.0;
		mTextureCoords[i + 0][1] = 1.0;
		// Top right
		mTextureCoords[i + 1][0] = 1.0;
		mTextureCoords[i + 1][1] = 0.0;
		// Top left
		mTextureCoords[i + 2][0] = 0.0;
		mTextureCoords[i + 2][1] = 0.0;
		// Bottom left
		mTextureCoords[i + 3][0] = 0.0;
		mTextureCoords[i + 3][1] = 1.0;
	}
}

CQuickSpriteSystem::~CQuickSpriteSystem(void)
{
}

void CQuickSpriteSystem::Flush(void)
{
	if (mNextVert==0)
	{
		return;
	}

	// TODO(M7): surface sprites were drawn as immediate client-state quads
	// (GL_QUADS + glLockArraysEXT); port to indexed triangles through the
	// streaming buffer (plan sections 1.3 and 1.6, tr_quicksprite).
	mNextVert = 0;
}


void CQuickSpriteSystem::StartGroup(textureBundle_t *bundle, uint32_t glbits, int fogIndex )
{
	mNextVert = 0;

	mTexBundle = bundle;
	mGLStateBits = glbits;
	if (fogIndex != -1)
	{
		mUseFog = qtrue;
		mFogIndex = fogIndex;
	}
	else
	{
		mUseFog = qfalse;
	}

	int cullingOn;
	qglGetIntegerv(GL_CULL_FACE,&cullingOn);

	if(cullingOn)
	{
		mTurnCullBackOn=qtrue;
	}
	else
	{
		mTurnCullBackOn=qfalse;
	}
	qglDisable(GL_CULL_FACE);
}


void CQuickSpriteSystem::EndGroup(void)
{
	Flush();

	// TODO(M3): fixed-function current color is gone; vertex colors come
	// from the tess buffer in ES3.
	if(mTurnCullBackOn)
	{
		qglEnable(GL_CULL_FACE);
	}
}




void CQuickSpriteSystem::Add(float *pointdata, color4ub_t color, vec2_t fog)
{
	float *curcoord;
	float *curfogtexcoord;
	uint32_t *curcolor;

	if (mNextVert>SHADER_MAX_VERTEXES-4)
	{
		Flush();
	}

	curcoord = mVerts[mNextVert];
	// This is 16*sizeof(float) because, pointdata comes from a float[16]
	memcpy(curcoord, pointdata, 16*sizeof(float));

	// Set up color
	curcolor = &mColors[mNextVert];
	*curcolor++ = *(uint32_t *)color;
	*curcolor++ = *(uint32_t *)color;
	*curcolor++ = *(uint32_t *)color;
	*curcolor++ = *(uint32_t *)color;

	if (fog)
	{
		curfogtexcoord = &mFogTextureCoords[mNextVert][0];
		*curfogtexcoord++ = fog[0];
		*curfogtexcoord++ = fog[1];

		*curfogtexcoord++ = fog[0];
		*curfogtexcoord++ = fog[1];

		*curfogtexcoord++ = fog[0];
		*curfogtexcoord++ = fog[1];

		*curfogtexcoord++ = fog[0];
		*curfogtexcoord++ = fog[1];

		mUseFog=qtrue;
	}
	else
	{
		mUseFog=qfalse;
	}

	mNextVert+=4;
}
