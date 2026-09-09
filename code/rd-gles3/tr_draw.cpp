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

// tr_draw.c
#include "../server/exe_headers.h"
#include "tr_common.h"
#include "tr_local.h"

/*
=============
RE_StretchRaw

Stretches a raw 32 bit power of 2 bitmap image over the given screen rectangle.
Used for cinematics.
=============
*/
void RE_StretchRaw ( int x, int y, int w, int h, int cols, int rows, const byte *data, int iClient, qboolean bDirty )
{
	int			i, j;
	int			start, end;

	if (!tr.registered) {
		return;
	}

	start = 0;
	if (r_speeds->integer) {
		start = ri.Milliseconds() * ri.Cvar_VariableValue("timescale");
	}

	// make sure rows and cols are powers of 2
	for (i = 0; (1 << i) < cols; i++)
	{
		;
	}
	for (j = 0; (1 << j) < rows; j++)
	{
		;
	}

	if ((1 << i) != cols || (1 << j) != rows) {
		Com_Error(ERR_DROP, "Draw_StretchRaw: size not a power of 2: %i by %i", cols, rows);
	}

	RE_UploadCinematic( cols, rows, (byte*)data, iClient, bDirty );

	if (r_speeds->integer) {
		end = ri.Milliseconds() * ri.Cvar_VariableValue("timescale");
		ri.Printf(PRINT_ALL, "RE_UploadCinematic( %i, %i ): %i msec\n", cols, rows, end - start);
	}

	tr.cinematicShader->stages[0]->bundle[0].image[0] = tr.scratchImage[iClient];
	RE_StretchPic(x, y, w, h, 0.5f / cols, 0.5f / rows, 1.0f - 0.5f / cols, 1.0f - 0.5 / rows, tr.cinematicShader->index);
}

extern byte *RB_ReadPixels(int x, int y, int width, int height, size_t *offset, int *padlen, int lineAlign);
void RE_GetScreenShot(byte *buffer, int w, int h)
{
	byte		*source;
	byte		*src, *dst;
	size_t offset = 0, memcount;
	int padlen;

	int			x, y;
	int			r, g, b;
	float		xScale, yScale;
	int			xx, yy;


	source = RB_ReadPixels(0, 0, glConfig.vidWidth, glConfig.vidHeight, &offset, &padlen, 0);
	memcount = (glConfig.vidWidth * 3 + padlen) * glConfig.vidHeight;

	// gamma correct
	if(glConfig.deviceSupportsGamma && !glConfigExt.doGammaCorrectionWithShaders)
		R_GammaCorrect(source + offset, memcount);

	// resample from source
	xScale = glConfig.vidWidth / (4.0*w);
	yScale = glConfig.vidHeight / (3.0*h);
	for ( y = 0 ; y < h ; y++ ) {
		for ( x = 0 ; x < w ; x++ ) {
			r = g = b = 0;
			for ( yy = 0 ; yy < 3 ; yy++ ) {
				for ( xx = 0 ; xx < 4 ; xx++ ) {
					src = source + offset + 3 * ( glConfig.vidWidth * (int)( (y*3+yy)*yScale ) + (int)( (x*4+xx)*xScale ) );
					r += src[0];
					g += src[1];
					b += src[2];
				}
			}
			dst = buffer + 4 * ((h - y - 1) * w + x );
			dst[0] = r / 12;
			dst[1] = g / 12;
			dst[2] = b / 12;
			dst[3] = 255;
		}
	}

	R_Free(source);
}

// this is just a chunk of code from RE_TempRawImage_ReadFromFile() below, subroutinised so I can call it
//	from the screen dissolve code as well...
//
static byte *RE_ReSample(byte *pbLoadedPic,			int iLoadedWidth,	int iLoadedHeight,
						 byte *pbReSampleBuffer,	int *piWidth,		int *piHeight
						)
{
	byte *pbReturn = NULL;

	// if not resampling, just return some values and return...
	//
	if ( pbReSampleBuffer == NULL || (iLoadedWidth == *piWidth && iLoadedHeight == *piHeight) )
	{
		// if not resampling, we're done, just return the loaded size...
		//
		*piWidth = iLoadedWidth;
		*piHeight= iLoadedHeight;
		pbReturn = pbLoadedPic;
	}
	else
	{
		// resample from pbLoadedPic to pbReSampledBuffer...
		//
		float	fXStep = (float)iLoadedWidth / (float)*piWidth;
		float	fYStep = (float)iLoadedHeight/ (float)*piHeight;
		int		iTotPixelsPerDownSample = (int)ceil(fXStep) * (int)ceil(fYStep);

		int 	r,g,b;

		byte	*pbDst = pbReSampleBuffer;

		for ( int y=0; y<*piHeight; y++ )
		{
			for ( int x=0; x<*piWidth; x++ )
			{
				r=g=b=0;

				for ( float yy = (float)y*fYStep; yy < (float)(y+1)*fYStep ; yy+=1 )
				{
					for ( float xx = (float)x*fXStep; xx < (float)(x+1)*fXStep ; xx+=1 )
					{
						byte *pbSrc = pbLoadedPic + 4 * ( ((int)yy * iLoadedWidth) + (int)xx );

						assert(pbSrc < pbLoadedPic + (iLoadedWidth * iLoadedHeight * 4) );

						r += pbSrc[0];
						g += pbSrc[1];
						b += pbSrc[2];
					}
				}

				assert(pbDst < pbReSampleBuffer + (*piWidth * *piHeight * 4));

				pbDst[0] = r / iTotPixelsPerDownSample;
				pbDst[1] = g / iTotPixelsPerDownSample;
				pbDst[2] = b / iTotPixelsPerDownSample;
				pbDst[3] = 255;
				pbDst += 4;
			}
		}

		// set return value...
		//
		pbReturn = pbReSampleBuffer;
	}

	return pbReturn;
}


// this is so the server (or anyone else) can get access to raw pixels if they really need to,
//	currently it's only used by the server so that savegames can embed a graphic in the auto-save files
//	(which can't do a screenshot since they're saved out before the level is drawn).
//
// by default, the pic will be returned as the original dims, but if pbReSampleBuffer != NULL then it's assumed to
//	be a big enough buffer to hold the resampled image, which also means that the width and height params are read as
//	inputs (as well as still being inherently outputs) and the pic is scaled to that size, and to that buffer.
//
// the return value is either NULL, or a pointer to the pixels to use (which may be either the pbReSampleBuffer param,
//	or the local ptr below).
//
// In either case, you MUST call the free-up function afterwards ( RE_TempRawImage_CleanUp() ) to get rid of any temp
//	memory after you've finished with the pic.
//
// Note: ALWAYS use the return value if != NULL, even if you passed in a declared resample buffer. This is because the
// resample will be skipped if the values you want are the same size as the pic that it loaded, so it'll return a
// different buffer.
//
// the vertflip param is used for those functions that expect things in OpenGL's upside-down pixel-read format (sigh)
//
// (not brilliantly fast, but it's only used for weird stuff anyway)
//
byte* pbLoadedPic = NULL;

void RE_TempRawImage_CleanUp(void);

byte* RE_TempRawImage_ReadFromFile(const char *psLocalFilename, int *piWidth, int *piHeight, byte *pbReSampleBuffer, qboolean qbVertFlip)
{
	RE_TempRawImage_CleanUp();	// jic

	byte *pbReturn = NULL;

	if (psLocalFilename && piWidth && piHeight)
	{
		int	 iLoadedWidth, iLoadedHeight;

		R_LoadImage( psLocalFilename, &pbLoadedPic, &iLoadedWidth, &iLoadedHeight);
		if ( pbLoadedPic )
		{
			pbReturn = RE_ReSample(	pbLoadedPic,		iLoadedWidth,	iLoadedHeight,
									pbReSampleBuffer,	piWidth,		piHeight);
		}
	}

	if (pbReturn && qbVertFlip)
	{
		unsigned int *pSrcLine = (unsigned int *) pbReturn;
		unsigned int *pDstLine = (unsigned int *) pbReturn + (*piHeight * *piWidth );	// *4 done by compiler (longs)
					   pDstLine-= *piWidth;	// point at start of last line, not first after buffer

		for (int iLineCount=0; iLineCount<*piHeight/2; iLineCount++)
		{
			for (int x=0; x<*piWidth; x++)
			{
				unsigned int l = pSrcLine[x];
				pSrcLine[x] = pDstLine[x];
				pDstLine[x] = l;
			}
			pSrcLine += *piWidth;
			pDstLine -= *piWidth;
		}
	}

	return pbReturn;
}

void RE_TempRawImage_CleanUp(void)
{
	if ( pbLoadedPic )
	{
		R_Free( pbLoadedPic );
		pbLoadedPic = NULL;
	}
}

// TODO(V3): the GL dissolve-wipe path (RE_InitDissolve/RE_ProcessDissolve) was not ported to
//	Vulkan yet; the stubs below simply disable the effect. Port needs: screen grab (RB_ReadPixels),
//	R_CreateImage + textured 2D quads through the vk backend.
qboolean RE_ProcessDissolve(void)
{
	return qfalse;
}

// return = qtrue(success) else fail, for those interested...
//
qboolean RE_InitDissolve(qboolean bForceCircularExtroWipe)
{
	return qfalse;
}
