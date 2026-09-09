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

// tr_subs.cpp - common function replacements for modular renderer
#include "tr_local.h"

void QDECL Com_Printf( const char *msg, ... )
{
	va_list         argptr;
	char            text[1024];

	va_start(argptr, msg);
	Q_vsnprintf(text, sizeof(text), msg, argptr);
	va_end(argptr);

	ri.Printf(PRINT_ALL, "%s", text);
}

void QDECL Com_OPrintf( const char *msg, ... )
{
	va_list         argptr;
	char            text[1024];

	va_start(argptr, msg);
	Q_vsnprintf(text, sizeof(text), msg, argptr);
	va_end(argptr);

	// SP refimport has no OPrintf; route to the standard print channel.
	ri.Printf(PRINT_ALL, "%s", text);
}

void QDECL Com_Error( int level, const char *error, ... )
{
	va_list         argptr;
	char            text[1024];

	va_start(argptr, error);
	Q_vsnprintf(text, sizeof(text), error, argptr);
	va_end(argptr);

	ri.Error(level, "%s", text);
}

// HUNK
// SP has no engine-side hunk in refimport; the renderer manages its own
// persistent allocations through ri.Malloc (TAG_HUNKALLOC) and temp buffers
// through TAG_TEMP_WORKSPACE, mirroring rd-vanilla/rd-gles3 behaviour.
void *Hunk_AllocateTempMemory( int size ) {
	return R_Malloc( size, TAG_TEMP_WORKSPACE, qfalse );
}

void Hunk_FreeTempMemory( void *buf ) {
	R_Free( buf );
}

int Hunk_MemoryRemaining( void ) {
	// SP zone has no queryable hunk; keep call sites functional.
	return ri.Z_MemSize( TAG_HUNKALLOC );
}

void *R_Malloc( int iSize, memtag_t eTag, qboolean bZeroit ) {
	return ri.Malloc( iSize, eTag, bZeroit, 4 );
}

void R_Free( void *ptr ) {
	ri.Z_Free( ptr );
}

int R_MemSize( memtag_t eTag ) {
	return ri.Z_MemSize( eTag );
}

void R_MorphMallocTag( void *pvBuffer, memtag_t eDesiredTag ) {
	ri.Z_MorphMallocTag( pvBuffer, eDesiredTag );
}

void *R_Hunk_Alloc( int iSize, qboolean bZeroit ) {
	return ri.Malloc( iSize, TAG_HUNKALLOC, bZeroit, 4 );
}

// SP refimport has no parse-line hook; used only for shader warning logs.
int COM_GetCurrentParseLine( void ) {
	return 0;
}

// ZONE
// These definitions intentionally match the prototypes already declared in
// SP qcommon.h; the bodies route through the SP refimport instead of the
// engine-side zone allocator.
void *Z_Malloc( int iSize, memtag_t eTag, qboolean bZeroit, int iAlign ) {
	return ri.Malloc( iSize, eTag, bZeroit, iAlign );
}

int Z_Free( void *ptr ) {
	return ri.Z_Free( ptr );
}

int Z_MemSize( memtag_t eTag ) {
	return ri.Z_MemSize( eTag );
}

void Z_MorphMallocTag( void *pvAddress, memtag_t eDesiredTag ) {
	ri.Z_MorphMallocTag( pvAddress, eDesiredTag );
}
