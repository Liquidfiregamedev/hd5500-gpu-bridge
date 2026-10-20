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
*/
#pragma once

#include <d3dcommon.h>

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

	inline void summary()
	{
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
