/*
This file belongs to Ashes.
See LICENSE file in root folder.

v12: bounded in-process cache of D3DCompile output.

The key is the complete compiler input that affects the output: the full HLSL
source text, entry point, target profile and compile flags.  A hit therefore
returns bytecode identical to what D3DCompile would produce for that input.
Blobs are immutable and shared through COM reference counting.

Toggles:
  RW_HD5500_SHADER_CACHE=0      disable (every module compiles, as in v11)
  RW_HD5500_SHADER_CACHE_MB=N   byte budget for keys + bytecode (default 32)

v13.22: persistent disk layer. The in-process cache only helps within one run; every
launch still compiled every shader with D3DCompile (optimisation level 1) on the CPU,
which is what freezes map initialisation on a 2-core laptop. Compiled bytecode is now
also stored on disk and reused on the next launch.
  RW_GPU_SHADER_DISK_CACHE=<dir>  location (default %LOCALAPPDATA%\HD5500-GPU-Bridge\shadercache)
  RW_GPU_SHADER_DISK_CACHE=0      disable the disk layer
File name = 64-bit FNV-1a of the full key; the file stores the full key and is only used
when it matches byte for byte, so a hash collision can never return wrong bytecode.
Writes go to a temporary file that is renamed into place (no torn files when two
processes or threads write at once).
*/
#pragma once

#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ashes::D3D11_NAMESPACE::shadercache
{
	struct Entry
	{
		std::string key;
		ID3DBlob * blob{};
		size_t bytes{};
	};

	struct State
	{
		std::mutex mutex;
		std::list< Entry > lru; // front = most recently used
		std::unordered_map< std::string_view, std::list< Entry >::iterator > index;
		size_t bytes{};
		uint64_t hits{};
		uint64_t misses{};
		uint64_t inserts{};
		uint64_t evictions{};
		uint64_t rejected{};
		// Intentionally no Release() in a destructor: at process exit the blob's
		// implementing DLL (d3dcompiler) may already be detached. The OS reclaims
		// the memory; evictions release blobs normally while the process runs.
	};

	inline State & state()
	{
		static State result;
		return result;
	}

	inline bool enabled()
	{
		static bool const value = []()
		{
			char text[4]{};
			auto const length = GetEnvironmentVariableA( "RW_HD5500_SHADER_CACHE", text, DWORD( sizeof( text ) ) );
			return !( length == 1u && text[0] == '0' );
		}();
		return value;
	}

	inline size_t capBytes()
	{
		static size_t const value = []()
		{
			char text[16]{};
			auto const length = GetEnvironmentVariableA( "RW_HD5500_SHADER_CACHE_MB", text, DWORD( sizeof( text ) ) );
			uint64_t mib = 32u;
			if ( length > 0u && length < sizeof( text ) )
			{
				mib = std::strtoull( text, nullptr, 10 );
			}
			return size_t( mib * 1024ull * 1024ull );
		}();
		return value;
	}

	inline std::string makeKey( std::string const & source
		, char const * entryPoint
		, char const * profile
		, unsigned flags )
	{
		std::string key;
		key.reserve( source.size() + 96u );
		key += profile ? profile : "";
		key.push_back( '\0' );
		key += entryPoint ? entryPoint : "";
		key.push_back( '\0' );
		key += std::to_string( flags );
		key.push_back( '\0' );
		key += source;
		return key;
	}

	// Returns an AddRef'd blob on a hit, nullptr otherwise.
	inline ID3DBlob * find( std::string const & key )
	{
		if ( !enabled() || !capBytes() ) return nullptr;
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		auto it = s.index.find( std::string_view{ key } );
		if ( it == s.index.end() )
		{
			++s.misses;
			return nullptr;
		}
		++s.hits;
		s.lru.splice( s.lru.begin(), s.lru, it->second );
		it->second->blob->AddRef();
		return it->second->blob;
	}

	inline void insert( std::string key, ID3DBlob * blob )
	{
		if ( !enabled() || !blob ) return;
		auto const cap = capBytes();
		auto const bytes = key.size() + size_t( blob->GetBufferSize() );
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		if ( bytes > cap )
		{
			++s.rejected;
			return;
		}
		if ( s.index.find( std::string_view{ key } ) != s.index.end() )
		{
			return; // another thread inserted the same input meanwhile
		}
		while ( s.bytes + bytes > cap && !s.lru.empty() )
		{
			auto & victim = s.lru.back();
			s.index.erase( std::string_view{ victim.key } );
			s.bytes -= victim.bytes;
			if ( victim.blob ) victim.blob->Release();
			s.lru.pop_back();
			++s.evictions;
		}
		blob->AddRef();
		s.lru.push_front( Entry{ std::move( key ), blob, bytes } );
		s.index.emplace( std::string_view{ s.lru.front().key }, s.lru.begin() );
		s.bytes += bytes;
		++s.inserts;
	}

	//*********************************************************************************************
	// v13.22 disk layer
	//*********************************************************************************************

	struct DiskStats
	{
		std::mutex mutex;
		uint64_t hits{};
		uint64_t misses{};
		uint64_t writes{};
		uint64_t failures{};
		double compileMs{};      // time spent in D3DCompile this run
		uint64_t compiles{};
		double slowestMs{};
		// v13.64: shader modules made this run and where their time went
		uint64_t modules{}, prewarmed{};
		double translateMs{}, bytecodeMs{}, createMs{};
	};

	inline DiskStats & diskStats()
	{
		static DiskStats result;
		return result;
	}

	inline std::string const & diskDir()
	{
		static std::string const value = []()
		{
			char text[MAX_PATH]{};
			auto length = GetEnvironmentVariableA( "RW_GPU_SHADER_DISK_CACHE", text, DWORD( sizeof( text ) ) );
			std::string dir;
			if ( length == 1u && text[0] == '0' ) return dir;
			if ( length > 0u && length < sizeof( text ) )
			{
				dir = text;
			}
			else
			{
				length = GetEnvironmentVariableA( "LOCALAPPDATA", text, DWORD( sizeof( text ) ) );
				if ( length == 0u || length >= sizeof( text ) ) return dir;
				dir = std::string{ text } + "\\HD5500-GPU-Bridge";
				CreateDirectoryA( dir.c_str(), nullptr );
				dir += "\\shadercache";
			}
			CreateDirectoryA( dir.c_str(), nullptr );
			auto attributes = GetFileAttributesA( dir.c_str() );
			if ( attributes == INVALID_FILE_ATTRIBUTES || !( attributes & FILE_ATTRIBUTE_DIRECTORY ) )
			{
				std::fprintf( stderr, "[RW-GPU-SHADERCACHE] disk cache unavailable at %s\n", dir.c_str() );
				dir.clear();
			}
			else
			{
				std::fprintf( stderr, "[RW-GPU-SHADERCACHE] disk cache %s\n", dir.c_str() );
			}
			return dir;
		}();
		return value;
	}

	inline uint64_t fnv1a( std::string const & data )
	{
		uint64_t hash = 1469598103934665603ull;
		for ( unsigned char c : data )
		{
			hash ^= c;
			hash *= 1099511628211ull;
		}
		return hash;
	}

	inline std::string diskPath( std::string const & key )
	{
		char name[48];
		std::snprintf( name, sizeof( name ), "\\%016llx-%08x.dxbc"
			, static_cast< unsigned long long >( fnv1a( key ) ), unsigned( key.size() ) );
		return diskDir() + name;
	}

	static constexpr uint32_t DiskMagic = 0x31434448u; // "HDC1"

	// Returns an AddRef'd blob on a hit, nullptr otherwise.
	inline ID3DBlob * diskFind( std::string const & key )
	{
		if ( !enabled() || diskDir().empty() ) return nullptr;
		auto & st = diskStats();
		auto file = CreateFileA( diskPath( key ).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
		if ( file == INVALID_HANDLE_VALUE )
		{
			std::lock_guard< std::mutex > lock{ st.mutex };
			++st.misses;
			return nullptr;
		}
		ID3DBlob * blob{};
		uint32_t header[3]{};   // magic, key size, blob size
		DWORD read{};
		bool ok = ReadFile( file, header, sizeof( header ), &read, nullptr ) && read == sizeof( header )
			&& header[0] == DiskMagic && header[1] == key.size() && header[2] > 0u && header[2] < ( 64u << 20 );
		if ( ok )
		{
			std::string storedKey( header[1], '\0' );
			ok = ReadFile( file, storedKey.data(), header[1], &read, nullptr ) && read == header[1] && storedKey == key;
		}
		if ( ok ) ok = SUCCEEDED( D3DCreateBlob( header[2], &blob ) );
		if ( ok ) ok = ReadFile( file, blob->GetBufferPointer(), header[2], &read, nullptr ) && read == header[2];
		CloseHandle( file );
		std::lock_guard< std::mutex > lock{ st.mutex };
		if ( !ok )
		{
			if ( blob ) blob->Release();
			++st.misses;
			return nullptr;
		}
		if ( st.hits++ == 0u ) std::fprintf( stderr, "[RW-GPU-SHADERCACHE] first disk hit (shaders compiled in an earlier run are reused)\n" );
		return blob;
	}

	inline void diskStore( std::string const & key, ID3DBlob * blob )
	{
		if ( !enabled() || !blob || diskDir().empty() ) return;
		auto const path = diskPath( key );
		char tmp[64];
		std::snprintf( tmp, sizeof( tmp ), ".%lu.%lu.tmp", GetCurrentProcessId(), GetCurrentThreadId() );
		auto const tmpPath = path + tmp;
		auto file = CreateFileA( tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
		bool ok = file != INVALID_HANDLE_VALUE;
		if ( ok )
		{
			uint32_t header[3]{ DiskMagic, uint32_t( key.size() ), uint32_t( blob->GetBufferSize() ) };
			DWORD written{};
			ok = WriteFile( file, header, sizeof( header ), &written, nullptr ) && written == sizeof( header )
				&& WriteFile( file, key.data(), DWORD( key.size() ), &written, nullptr ) && written == key.size()
				&& WriteFile( file, blob->GetBufferPointer(), DWORD( blob->GetBufferSize() ), &written, nullptr ) && written == blob->GetBufferSize();
			CloseHandle( file );
			ok = ok && MoveFileExA( tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING );
			if ( !ok ) DeleteFileA( tmpPath.c_str() );
		}
		auto & st = diskStats();
		std::lock_guard< std::mutex > lock{ st.mutex };
		if ( ok ) ++st.writes; else ++st.failures;
	}

	inline void noteCompile( double ms )
	{
		auto & st = diskStats();
		std::lock_guard< std::mutex > lock{ st.mutex };
		st.compileMs += ms;
		++st.compiles;
		if ( ms > st.slowestMs ) st.slowestMs = ms;
	}

	inline void noteModule( double translateMs, double bytecodeMs, double createMs, bool prewarmed )
	{
		auto & st = diskStats();
		std::lock_guard< std::mutex > lock{ st.mutex };
		++st.modules;
		if ( prewarmed ) ++st.prewarmed;
		st.translateMs += translateMs;
		st.bytecodeMs += bytecodeMs;
		st.createMs += createMs;
	}

	inline void summary()
	{
		{
			auto & st = diskStats();
			std::lock_guard< std::mutex > lock{ st.mutex };
			std::fprintf( stderr, "[RW-GPU-SHADERCACHE] modules=%llu (prewarmed objects used %llu) translate=%.0fms bytecode=%.0fms create=%.0fms in total\n"
				, static_cast< unsigned long long >( st.modules ), static_cast< unsigned long long >( st.prewarmed ), st.translateMs, st.bytecodeMs, st.createMs );
		}
		{
			auto & st = diskStats();
			std::lock_guard< std::mutex > lock{ st.mutex };
			std::fprintf( stderr, "[RW-GPU-SHADERCACHE] disk hits=%llu misses=%llu writes=%llu failures=%llu compiles=%llu compileMs=%.0f slowestMs=%.0f\n"
				, static_cast< unsigned long long >( st.hits ), static_cast< unsigned long long >( st.misses )
				, static_cast< unsigned long long >( st.writes ), static_cast< unsigned long long >( st.failures )
				, static_cast< unsigned long long >( st.compiles ), st.compileMs, st.slowestMs );
		}
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		std::fprintf( stderr, "[RW-GPU-SHADERCACHE] enabled=%u cap=%llu bytes=%llu entries=%llu hits=%llu misses=%llu inserts=%llu evictions=%llu rejected=%llu\n"
			, unsigned( enabled() )
			, static_cast< unsigned long long >( capBytes() )
			, static_cast< unsigned long long >( s.bytes )
			, static_cast< unsigned long long >( s.lru.size() )
			, static_cast< unsigned long long >( s.hits )
			, static_cast< unsigned long long >( s.misses )
			, static_cast< unsigned long long >( s.inserts )
			, static_cast< unsigned long long >( s.evictions )
			, static_cast< unsigned long long >( s.rejected ) );
		std::fflush( stderr );
	}
}
