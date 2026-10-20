/*
v13.43: save/restore of the output-merger state INCLUDING unordered access views.

The bridge's helper passes (rectangle clears, draw-based blits, swizzle shadows) switch
render targets with OMSetRenderTargets and restore them the same way afterwards. On D3D11,
OMSetRenderTargets behaves like OMSetRenderTargetsAndUnorderedAccessViews with NumUAVs = 0:
it UNBINDS every UAV of the output merger. Graphics-stage UAVs are how the bridge exposes
writable storage buffers to vertex/pixel shaders - Waylith's instanced draws read their
per-instance matrices from such a buffer (`_InstanceTransforms`, RWByteAddressBuffer u1).
The command stream still believed those UAVs were bound (Context::uavs is only re-applied
when the set changes), so the next instanced draw read zeros: every instance collapsed to
a point and the draw vanished (RimWorld: the red "forbidden" X disappearing for seconds,
coming back when the descriptor set changed).

save() captures RTVs, DSV and the UAV slots; restore() puts all of them back.
RW_GPU_UAV_RESTORE=0 restores the old behaviour (RTVs/DSV only) for A/B tests.
*/
#pragma once

#include <d3d11.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace ashes::D3D11_NAMESPACE::outputstate
{
	static constexpr UINT UavSlots = D3D11_PS_CS_UAV_REGISTER_COUNT;   // 8 on FL11_0; enough for the bridge's graphics UAVs

	inline bool enabled()
	{
		static bool const value = []()
		{
			auto env = std::getenv( "RW_GPU_UAV_RESTORE" );
			return !( env && env[0] == '0' );
		}();
		return value;
	}

	inline void save( ID3D11DeviceContext * ctx
		, ID3D11RenderTargetView * ( &rtvs )[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]
		, ID3D11DepthStencilView ** dsv
		, ID3D11UnorderedAccessView * ( &uavs )[UavSlots] )
	{
		for ( auto & uav : uavs ) uav = nullptr;
		if ( enabled() )
		{
			ctx->OMGetRenderTargetsAndUnorderedAccessViews( D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv, 0u, UavSlots, uavs );
		}
		else
		{
			ctx->OMGetRenderTargets( D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv );
		}
	}

	inline void restore( ID3D11DeviceContext * ctx
		, ID3D11RenderTargetView * ( &rtvs )[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]
		, ID3D11DepthStencilView * dsv
		, ID3D11UnorderedAccessView * ( &uavs )[UavSlots] )
	{
		UINT rtvCount = 0u;
		for ( UINT i = 0u; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i ) if ( rtvs[i] ) rtvCount = i + 1u;
		UINT uavEnd = 0u;
		for ( UINT i = rtvCount; i < UavSlots; ++i ) if ( uavs[i] ) uavEnd = i + 1u;
		if ( !enabled() || uavEnd == 0u )
		{
			ctx->OMSetRenderTargets( D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv );
		}
		else
		{
			ctx->OMSetRenderTargetsAndUnorderedAccessViews( rtvCount, rtvs, dsv, rtvCount, uavEnd - rtvCount, uavs + rtvCount, nullptr );
			static std::atomic< uint32_t > logged{ 0u };
			auto n = ++logged;
			if ( n <= 4u || ( n & ( n - 1u ) ) == 0u )
				std::fprintf( stderr, "[RW-GPU-UAV-RESTORE] count=%u rtvs=%u uavSlots=%u..%u (graphics UAVs re-bound after a helper pass)\n", n, rtvCount, rtvCount, uavEnd - 1u );
		}
		for ( auto & uav : uavs ) { if ( uav ) uav->Release(); uav = nullptr; }
	}
}
