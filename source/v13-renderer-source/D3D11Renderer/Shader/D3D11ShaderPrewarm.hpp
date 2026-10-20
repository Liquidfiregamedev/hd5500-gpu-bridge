/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

// v13.64: shader pre-warming from the disk cache.
//
// Measured on the laptop: RimWorld creates many of its shader modules lazily, after the map is
// already on screen (shader-0005 onwards appear after the first frame statistics), so the first
// time a new kind of thing is drawn the frame also pays for reading the cached bytecode and for
// the driver's own compilation in Create*Shader (Intel compiles DXBC to GPU code there).
// DXVK's guidelines: compile shaders during loading; on-demand compiles cause worse stutter.
//
// At device creation a background thread reads every cached bytecode file (shaders this PC has
// translated in earlier runs), puts the bytecode into the in-memory cache and creates the D3D11
// shader object for it (the D3D11 device is free-threaded). When the game later creates the
// same shader module, the bridge still translates the SPIR-V (needed to form the cache key) but
// takes the ready shader object instead of reading the file and creating it again.
//   RW_GPU_SHADER_PREWARM=0  switches it off.

#include "Shader/D3D11ShaderBytecodeCache.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"

#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace ashes::D3D11_NAMESPACE::shaderprewarm
{
	struct State
	{
		std::mutex mutex;
		std::unordered_map< std::string, ID3D11DeviceChild * > objects;   // key -> AddRef'd shader object
		std::atomic< uint32_t > created{ 0u };
		std::atomic< uint32_t > used{ 0u };
		std::atomic< bool > done{ false };
		double ms{};
	};

	inline State & state()
	{
		static State * const result = new State{};   // never destroyed (detached thread, process exit)
		return *result;
	}

	inline bool enabled()
	{
		static bool const result = envflags::on( "RW_GPU_SHADER_PREWARM", true );
		return result;
	}

	// The key starts with the target profile ("vs_5_0", "ps_5_0", ...), see shadercache::makeKey.
	inline ID3D11DeviceChild * createFor( ID3D11Device * device, std::string const & key, ID3DBlob * blob )
	{
		void const * code = blob->GetBufferPointer();
		SIZE_T const size = blob->GetBufferSize();
		if ( key.size() < 3u ) return nullptr;
		HRESULT hr = E_FAIL;
		ID3D11DeviceChild * result{};
		switch ( key[0] )
		{
		case 'v': { ID3D11VertexShader * s{}; hr = device->CreateVertexShader( code, size, nullptr, &s ); result = s; break; }
		case 'p': { ID3D11PixelShader * s{}; hr = device->CreatePixelShader( code, size, nullptr, &s ); result = s; break; }
		case 'g': { ID3D11GeometryShader * s{}; hr = device->CreateGeometryShader( code, size, nullptr, &s ); result = s; break; }
		case 'h': { ID3D11HullShader * s{}; hr = device->CreateHullShader( code, size, nullptr, &s ); result = s; break; }
		case 'd': { ID3D11DomainShader * s{}; hr = device->CreateDomainShader( code, size, nullptr, &s ); result = s; break; }
		case 'c': { ID3D11ComputeShader * s{}; hr = device->CreateComputeShader( code, size, nullptr, &s ); result = s; break; }
		default: break;
		}
		if ( FAILED( hr ) )
		{
			if ( result ) result->Release();
			return nullptr;
		}
		return result;
	}

	inline void run( ID3D11Device * device )
	{
		LARGE_INTEGER f{}, a{}, b{};
		QueryPerformanceFrequency( &f );
		QueryPerformanceCounter( &a );
		SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );   // never compete with the game
		auto & s = state();
		std::string const pattern = shadercache::diskDir() + "\\*.dxbc";
		WIN32_FIND_DATAA data{};
		HANDLE find = FindFirstFileA( pattern.c_str(), &data );
		uint32_t files = 0u;
		if ( find != INVALID_HANDLE_VALUE )
		{
			do
			{
				if ( ++files > 1024u ) break;
				std::string const path = shadercache::diskDir() + "\\" + data.cFileName;
				HANDLE file = CreateFileA( path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
				if ( file == INVALID_HANDLE_VALUE ) continue;
				uint32_t header[3]{};
				DWORD read{};
				bool ok = ReadFile( file, header, sizeof( header ), &read, nullptr ) && read == sizeof( header )
					&& header[0] == shadercache::DiskMagic && header[1] > 0u && header[1] < ( 4u << 20 ) && header[2] > 0u && header[2] < ( 64u << 20 );
				std::string key;
				ID3DBlob * blob{};
				if ( ok )
				{
					key.resize( header[1] );
					ok = ReadFile( file, key.data(), header[1], &read, nullptr ) && read == header[1];
				}
				if ( ok ) ok = SUCCEEDED( D3DCreateBlob( header[2], &blob ) );
				if ( ok ) ok = ReadFile( file, blob->GetBufferPointer(), header[2], &read, nullptr ) && read == header[2];
				CloseHandle( file );
				// A stale file (other flags/profile) simply never matches a key; a corrupt one is skipped.
				if ( ok && shadercache::diskPath( key ) == path )
				{
					if ( auto object = createFor( device, key, blob ) )
					{
						shadercache::insert( key, blob );
						std::lock_guard< std::mutex > lock{ s.mutex };
						if ( !s.objects.emplace( std::move( key ), object ).second ) object->Release();
						else ++s.created;
					}
				}
				if ( blob ) blob->Release();
			}
			while ( FindNextFileA( find, &data ) );
			FindClose( find );
		}
		QueryPerformanceCounter( &b );
		s.ms = f.QuadPart ? double( b.QuadPart - a.QuadPart ) * 1000.0 / double( f.QuadPart ) : 0.0;
		std::fprintf( stderr, "[RW-GPU-SHADERCACHE] prewarm: %u shader objects created from the disk cache in %.0f ms (background)\n"
			, unsigned( s.created.load() ), s.ms );
		s.done = true;
		device->Release();
	}

	// Once per process, at device creation.
	inline void start( ID3D11Device * device )
	{
		static std::atomic< bool > started{ false };
		if ( !device || !enabled() || !shadercache::enabled() || shadercache::diskDir().empty() || started.exchange( true ) ) return;
		device->AddRef();
		std::thread{ [device]() { run( device ); } }.detach();
	}

	// Returns an AddRef'd shader object for this cache key, or nullptr.
	inline ID3D11DeviceChild * take( ID3D11Device * device, std::string const & key )
	{
		if ( !enabled() || key.empty() ) return nullptr;
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		auto it = s.objects.find( key );
		if ( it == s.objects.end() ) return nullptr;
		ID3D11Device * owner{};
		it->second->GetDevice( &owner );
		bool const sameDevice = owner == device;
		if ( owner ) owner->Release();
		if ( !sameDevice ) return nullptr;
		it->second->AddRef();
		++s.used;
		return it->second;
	}
}
