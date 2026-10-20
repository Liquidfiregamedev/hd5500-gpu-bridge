/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

// v13.65: GPU compatibility report ("which GPU is this, what can it do, what will be degraded").
//
// Idea borrowed from DXVK-Sarek (a DXVK fork for older GPUs): log every optional capability at
// device creation and mark what is missing as DEGRADED, so a report from another PC tells at a
// glance whether a problem is "this GPU cannot do X" or a bug. The bridge itself is generic: any
// Direct3D 11 GPU with feature level 11_0 or higher should run it (Intel Ivy Bridge/Haswell/
// Broadwell, NVIDIA Fermi/Kepler, AMD TeraScale 2/GCN); only the HD 5500 is tested.
// One block of [RW-GPU-COMPAT] lines on stderr, once per process. Read-only queries; nothing is
// changed.

#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>
#include <cstdio>

namespace ashes::D3D11_NAMESPACE::compat
{
	inline char const * vendorName( UINT vendor )
	{
		switch ( vendor )
		{
		case 0x8086: return "Intel";
		case 0x10DE: return "NVIDIA";
		case 0x1002: case 0x1022: return "AMD";
		case 0x1414: return "Microsoft (WARP software renderer)";
		case 0x5143: return "Qualcomm";
		default: return "unknown vendor";
		}
	}

	inline char const * levelName( D3D_FEATURE_LEVEL level )
	{
		switch ( level )
		{
		case D3D_FEATURE_LEVEL_12_1: return "12_1";
		case D3D_FEATURE_LEVEL_12_0: return "12_0";
		case D3D_FEATURE_LEVEL_11_1: return "11_1";
		case D3D_FEATURE_LEVEL_11_0: return "11_0";
		case D3D_FEATURE_LEVEL_10_1: return "10_1";
		case D3D_FEATURE_LEVEL_10_0: return "10_0";
		default: return "9_x";
		}
	}

	inline void report( ID3D11Device * device, IDXGIAdapter * adapter, D3D_FEATURE_LEVEL level )
	{
		static std::atomic< bool > done{ false };
		if ( !device || done.exchange( true ) ) return;

		DXGI_ADAPTER_DESC desc{};
		if ( adapter ) adapter->GetDesc( &desc );
		LARGE_INTEGER umd{};
		bool const haveUmd = adapter && SUCCEEDED( adapter->CheckInterfaceSupport( __uuidof( IDXGIDevice ), &umd ) );
		unsigned const v1 = unsigned( HIWORD( umd.HighPart ) ), v2 = unsigned( LOWORD( umd.HighPart ) );
		unsigned const v3 = unsigned( HIWORD( umd.LowPart ) ), v4 = unsigned( LOWORD( umd.LowPart ) );

		std::fprintf( stderr, "[RW-GPU-COMPAT] adapter=\"%ls\" vendor=%s (0x%04X) device=0x%04X driver=%s%u.%u.%u.%u feature level %s, video memory %u MB dedicated / %u MB shared\n"
			, adapter ? desc.Description : L"(default)", vendorName( desc.VendorId ), unsigned( desc.VendorId ), unsigned( desc.DeviceId )
			, haveUmd ? "" : "unknown ", v1, v2, v3, v4, levelName( level )
			, unsigned( desc.DedicatedVideoMemory >> 20 ), unsigned( desc.SharedSystemMemory >> 20 ) );

		D3D11_FEATURE_DATA_THREADING threading{};
		device->CheckFeatureSupport( D3D11_FEATURE_THREADING, &threading, sizeof( threading ) );
		D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
		device->CheckFeatureSupport( D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof( options ) );
		D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hw10{};
		device->CheckFeatureSupport( D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &hw10, sizeof( hw10 ) );
		std::fprintf( stderr, "[RW-GPU-COMPAT] caps: driverCommandLists=%d concurrentCreates=%d constantBufferOffsetting=%d constantBufferPartialUpdate=%d noOverwriteDynamicCB=%d noOverwriteDynamicSRV=%d logicOp=%d computeOn10x=%d\n"
			, int( threading.DriverCommandLists ), int( threading.DriverConcurrentCreates )
			, int( options.ConstantBufferOffsetting ), int( options.ConstantBufferPartialUpdate )
			, int( options.MapNoOverwriteOnDynamicConstantBuffer ), int( options.MapNoOverwriteOnDynamicBufferSRV )
			, int( options.OutputMergerLogicOp ), int( hw10.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x ) );

		// What will be missing or slower on this GPU (DXVK-Sarek style "DEGRADED" notes).
		unsigned degraded = 0u;
		auto note = [&degraded]( char const * text )
		{
			++degraded;
			std::fprintf( stderr, "[RW-GPU-COMPAT] DEGRADED: %s\n", text );
		};
		if ( level < D3D_FEATURE_LEVEL_11_0 )
			note( "feature level below 11_0: no compute shaders or structured buffers in all stages; RimWorld 1.7 will most likely not start or draw" );
		else if ( level < D3D_FEATURE_LEVEL_11_1 )
			note( "feature level 11_0 (not 11_1): writable storage buffers in vertex shaders need 11_1; draws that use one (e.g. some instanced overlays) may be missing; read-only storage buffers are fine" );
		if ( !options.MapNoOverwriteOnDynamicConstantBuffer || !options.MapNoOverwriteOnDynamicBufferSRV )
			note( "no partial updates of dynamic buffers (MAP_NO_OVERWRITE): uniform/storage uploads rewrite whole buffers (slower)" );
		if ( desc.VendorId == 0x1414 )
			note( "Microsoft WARP software renderer: correct pictures, but very slow; use only as a reference" );

		// Verdict, so a player with another GPU knows what to expect.
		char const * verdict = "NOT SUPPORTED (needs feature level 11_0)";
		if ( desc.VendorId == 0x8086 && desc.DeviceId == 0x1616 ) verdict = "SUPPORTED (tested: Intel HD 5500)";
		else if ( desc.VendorId == 0x1414 ) verdict = "REFERENCE ONLY (software)";
		else if ( level >= D3D_FEATURE_LEVEL_11_1 ) verdict = "EXPECTED TO WORK (feature level 11_1, untested on this GPU - please report)";
		else if ( level >= D3D_FEATURE_LEVEL_11_0 ) verdict = "SHOULD WORK WITH LIMITATIONS (feature level 11_0, untested - please report)";
		std::fprintf( stderr, "[RW-GPU-COMPAT] verdict: %s; %u degraded item(s)\n", verdict, degraded );
		std::fflush( stderr );
	}
}
