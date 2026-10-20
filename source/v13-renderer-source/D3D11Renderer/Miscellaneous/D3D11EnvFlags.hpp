/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

// v13.51: environment switches read ONCE per process.
// v13.62: one helper for all switches (GetEnvironmentVariableA = the process environment the
// launcher sets, the same source for every switch), plus the cached switches that are asked in
// per-command / per-upload paths.
//
// Rule for new code: never read the environment in a per-command path - add a cached accessor
// here (a function-local static) and call that.
//
// Usage:  if ( envflags::uiTrace() ) { ... }   or   static bool const x = envflags::on( "NAME", true );

#include <windows.h>
#include <cstdlib>

#include <cstddef>

namespace ashes::D3D11_NAMESPACE::envflags
{
	// Copies the value into `buffer` (NUL-terminated); returns its length, 0 when unset or too long.
	inline size_t text( char const * name, char * buffer, size_t size )
	{
		if ( !buffer || size == 0u ) return 0u;
		buffer[0] = 0;
		auto const length = GetEnvironmentVariableA( name, buffer, DWORD( size ) );
		if ( length == 0u || length >= size )
		{
			buffer[0] = 0;
			return 0u;
		}
		return size_t( length );
	}

	// True when the variable exists and its first character is '1'.
	inline bool readOne( char const * name )
	{
		char value[8]{};
		return text( name, value, sizeof( value ) ) > 0u && value[0] == '1';
	}

	// True when the variable exists and its first character is '0' (explicit "off").
	inline bool readZero( char const * name )
	{
		char value[8]{};
		return text( name, value, sizeof( value ) ) > 0u && value[0] == '0';
	}

	// A feature switch: `defaultOn` when unset; "0" switches off, "1" switches on.
	inline bool on( char const * name, bool defaultOn )
	{
		return defaultOn ? !readZero( name ) : readOne( name );
	}

	// True when the variable exists at all (any value).
	// v13.70: unsigned number from the environment, or the default when unset/invalid.
	inline unsigned long number( char const * name, unsigned long defaultValue )
	{
		char value[16]{};
		if ( text( name, value, sizeof( value ) ) == 0u ) return defaultValue;
		char * end{};
		unsigned long const result = std::strtoul( value, &end, 10 );
		return end != value ? result : defaultValue;
	}

	inline bool isSet( char const * name )
	{
		char value[8]{};
		return GetEnvironmentVariableA( name, value, DWORD( sizeof( value ) ) ) > 0u;
	}

	// ---- cached switches used in hot paths ----

	inline bool uiTrace()
	{
		static bool const result = readOne( "RW_HD5500_UI_TRACE" );
		return result;
	}

	inline bool textureTrace()
	{
		static bool const result = readOne( "RW_HD5500_TEXTURE_TRACE" );
		return result;
	}

	// RW_HD5500_COHERENT_REFRESH=0 switches off the refresh of mapped coherent memory.
	inline bool coherentRefreshOff()
	{
		static bool const result = readZero( "RW_HD5500_COHERENT_REFRESH" );
		return result;
	}

	// v13.24/v13.60: upload de-duplication (record time and execution time). RW_GPU_UPLOAD_DEDUPE=0 = off.
	inline bool uploadDedupe()
	{
		static bool const result = on( "RW_GPU_UPLOAD_DEDUPE", true );
		return result;
	}

	// v13.51b: post-submit cleanup de-duplication. RW_GPU_CLEANUP_DEDUPE=0 = off.
	inline bool cleanupDedupe()
	{
		static bool const result = on( "RW_GPU_CLEANUP_DEDUPE", true );
		return result;
	}

	// v13.62: these were read on EVERY direct texture upload / image release (up to ~3,000 per
	// map-load command buffer).
	inline bool uploadTrace()
	{
		static bool const result = isSet( "RW_GPU_UPLOAD_TRACE" );
		return result;
	}

	inline bool imageLifetimeTrace()
	{
		static bool const result = readOne( "RW_HD5500_IMAGE_LIFETIME_TRACE" );
		return result;
	}
}
