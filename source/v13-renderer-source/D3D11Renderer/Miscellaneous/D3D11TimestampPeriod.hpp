/*
	This file belongs to Ashes.
	See LICENSE file in root folder.
*/
#pragma once

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"

#include <cstdio>

namespace ashes::D3D11_NAMESPACE::timestampperiod
{
	// D3D11 timestamp values have units of GPU ticks.  Probe the adapter once
	// rather than claiming that one tick is one nanosecond.
	inline float measure( IDXGIAdapter * adapter, D3D_FEATURE_LEVEL requestedFeatureLevel )noexcept
	{
		ID3D11Device * device{};
		ID3D11DeviceContext * context{};
		ID3D11Query * query{};
		D3D_FEATURE_LEVEL actual{};
		D3D_FEATURE_LEVEL const levels[]{ requestedFeatureLevel };
		auto const fail = [&]( char const * reason )
		{
			std::fprintf( stderr, "[RW-GPU-TIMESTAMP] period fallback=1.0ns reason=%s\n", reason );
			safeRelease( query );
			safeRelease( context );
			safeRelease( device );
			return 1.0f;
		};

		if ( !adapter ) return fail( "no-adapter" );
		auto hr = D3D11CreateDevice( adapter
			, D3D_DRIVER_TYPE_UNKNOWN
			, nullptr
			, 0u
			, levels
			, 1u
			, D3D11_SDK_VERSION
			, &device
			, &actual
			, &context );
		if ( FAILED( hr ) || !device || !context ) return fail( "D3D11CreateDevice" );

		D3D11_QUERY_DESC desc{};
		desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		hr = device->CreateQuery( &desc, &query );
		if ( FAILED( hr ) || !query ) return fail( "CreateQuery" );
		context->Begin( query );
		context->End( query );
		context->Flush();

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result{};
		auto const deadline = GetTickCount64() + 1000u;
		for ( ;; )
		{
			hr = context->GetData( query, &result, UINT( sizeof( result ) ), D3D11_ASYNC_GETDATA_DONOTFLUSH );
			if ( hr == S_OK ) break;
			if ( FAILED( hr ) ) return fail( "GetData" );
			if ( GetTickCount64() >= deadline ) return fail( "GetData-timeout" );
			Sleep( 1u );
		}

		if ( result.Disjoint || !result.Frequency ) return fail( result.Disjoint ? "disjoint" : "zero-frequency" );
		auto const period = float( 1000000000.0 / double( result.Frequency ) );
		safeRelease( query );
		safeRelease( context );
		safeRelease( device );
		return period > 0.0f ? period : 1.0f;
	}
}
