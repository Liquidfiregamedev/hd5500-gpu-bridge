/*
This file belongs to Ashes.
See LICENSE file in root folder.

v13.14 (HD5500 GPU bridge): emulation of VkImageView component swizzles.

D3D11 shader resource views have no component mapping. Waylith stores opaque
greyscale textures as one channel (BC4 / R8) and samples them through a view
swizzle such as (R, R, R, ONE). Without emulation they sample as (r, 0, 0, 1),
i.e. pure red. Ashes advertised imageViewFormatSwizzle anyway.

Approach (generic, feature level 10.0): the first time a swizzled view is
bound for sampling, draw every mip/layer of the viewed range into a private
shadow texture through a pixel shader that applies the swizzle with exact
texel loads. The shadow is rebuilt whenever Image::getContentVersion() moves.
*/
#include "Miscellaneous/D3D11OutputState.hpp"
#include "Image/D3D11ImageView.hpp"

#include "Core/D3D11Device.hpp"
#include "Image/D3D11Image.hpp"

#include "ashesd3d11_api.hpp"

#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace ashes::D3D11_NAMESPACE
{
	namespace swz
	{
		struct Shared
		{
			ID3D11VertexShader * vs{};
			ID3D11PixelShader * ps{};
			ID3D11Buffer * cb{};
			ID3D11RasterizerState * rs{};
			ID3D11BlendState * bs{};
			ID3D11DepthStencilState * ds{};
			bool ok{};
		};

		static char const * const hlsl = R"(
cbuffer SwizzleCB : register(b0)
{
	uint4 sel; // per output channel: 0..3 = source R,G,B,A; 4 = 0; 5 = 1
};
Texture2DArray<float4> srcTex : register(t0);

float4 vsMain( uint id : SV_VertexID ) : SV_Position
{
	float2 p = float2( ( id << 1 ) & 2, id & 2 );
	return float4( p * float2( 2.0, -2.0 ) + float2( -1.0, 1.0 ), 0.0, 1.0 );
}

float pick( float4 c, uint s )
{
	return s < 4u ? c[s] : ( s == 4u ? 0.0 : 1.0 );
}

float4 psMain( float4 pos : SV_Position ) : SV_Target
{
	float4 c = srcTex.Load( int4( int2( pos.xy ), 0, 0 ) );
	return float4( pick( c, sel.x ), pick( c, sel.y ), pick( c, sel.z ), pick( c, sel.w ) );
}
)";

		static Shared const & getShared( ID3D11Device * d3d )
		{
			static std::mutex mutex;
			static std::map< ID3D11Device *, Shared > cache;
			std::lock_guard< std::mutex > lock{ mutex };
			auto & s = cache[d3d];
			if ( s.ok || s.vs ) return s;
			ID3DBlob * vsCode{}; ID3DBlob * psCode{}; ID3DBlob * err{};
			HRESULT hr = D3DCompile( hlsl, std::strlen( hlsl ), "swizzle.hlsl", nullptr, nullptr, "vsMain", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCode, &err );
			if ( FAILED( hr ) && err ) std::fprintf( stderr, "[RW-GPU-SWIZZLE] vs compile: %s\n", static_cast< char const * >( err->GetBufferPointer() ) );
			safeRelease( err );
			if ( SUCCEEDED( hr ) ) hr = D3DCompile( hlsl, std::strlen( hlsl ), "swizzle.hlsl", nullptr, nullptr, "psMain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, &err );
			if ( FAILED( hr ) && err ) std::fprintf( stderr, "[RW-GPU-SWIZZLE] ps compile: %s\n", static_cast< char const * >( err->GetBufferPointer() ) );
			safeRelease( err );
			if ( SUCCEEDED( hr ) ) hr = d3d->CreateVertexShader( vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &s.vs );
			if ( SUCCEEDED( hr ) ) hr = d3d->CreatePixelShader( psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &s.ps );
			safeRelease( vsCode ); safeRelease( psCode );
			if ( SUCCEEDED( hr ) )
			{
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = 16u;
				bd.Usage = D3D11_USAGE_DEFAULT;
				bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				hr = d3d->CreateBuffer( &bd, nullptr, &s.cb );
			}
			if ( SUCCEEDED( hr ) )
			{
				D3D11_RASTERIZER_DESC rd{};
				rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
				hr = d3d->CreateRasterizerState( &rd, &s.rs );
			}
			if ( SUCCEEDED( hr ) )
			{
				D3D11_BLEND_DESC bd{};
				bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
				hr = d3d->CreateBlendState( &bd, &s.bs );
			}
			if ( SUCCEEDED( hr ) )
			{
				D3D11_DEPTH_STENCIL_DESC dd{};
				hr = d3d->CreateDepthStencilState( &dd, &s.ds );
			}
			s.ok = SUCCEEDED( hr );
			if ( !s.ok ) std::fprintf( stderr, "[RW-GPU-SWIZZLE] shared object creation failed hr=0x%08lX\n", (unsigned long)hr );
			return s;
		}

		// Shadow format that loses nothing the source can hold.
		static DXGI_FORMAT shadowFormat( DXGI_FORMAT src )noexcept
		{
			switch ( src )
			{
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_A8_UNORM:
			case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			default:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			}
		}

		static std::atomic< uint32_t > logCount{ 0u };
	}

	ID3D11ShaderResourceView * ImageView::getSampledView( ID3D11DeviceContext * ctx, bool * rendered )const
	{
		if ( rendered ) *rendered = false;
		if ( !m_swizzled || m_swizzleFailed || !ctx ) return m_shaderView;
		auto image = get( m_createInfo.image );
		auto const version = image->getContentVersion();
		if ( m_swizzleView && m_swizzleVersion == version ) return m_swizzleView;

		auto d3d = get( m_device )->getDevice();
		auto const & shared = swz::getShared( d3d );
		D3D11_SHADER_RESOURCE_VIEW_DESC srcDesc{};
		m_shaderView->GetDesc( &srcDesc );
		bool const is2D = srcDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
		bool const is2DArray = srcDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		if ( !shared.ok || ( !is2D && !is2DArray ) )
		{
			m_swizzleFailed = true;
			std::fprintf( stderr, "[RW-GPU-SWIZZLE] unsupported view (dimension=%d sharedOk=%d) format=%d: swizzle ignored\n"
				, int( srcDesc.ViewDimension ), int( shared.ok ), int( m_createInfo.format ) );
			return m_shaderView;
		}
		auto const & range = m_createInfo.subresourceRange;
		UINT const mips = range.levelCount;
		UINT const layers = is2DArray ? range.layerCount : 1u;
		auto const & extent = image->getDimensions();
		UINT const width = std::max( 1u, extent.width >> range.baseMipLevel );
		UINT const height = std::max( 1u, extent.height >> range.baseMipLevel );

		if ( !m_swizzleTexture )
		{
			D3D11_TEXTURE2D_DESC td{};
			td.Width = width; td.Height = height; td.MipLevels = mips; td.ArraySize = layers;
			td.Format = swz::shadowFormat( srcDesc.Format );
			td.SampleDesc.Count = 1u;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			HRESULT hr = d3d->CreateTexture2D( &td, nullptr, &m_swizzleTexture );
			D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
			sv.Format = td.Format;
			if ( is2DArray )
			{
				sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				sv.Texture2DArray.MipLevels = mips; sv.Texture2DArray.ArraySize = layers;
			}
			else
			{
				sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				sv.Texture2D.MipLevels = mips;
			}
			if ( SUCCEEDED( hr ) ) hr = d3d->CreateShaderResourceView( m_swizzleTexture, &sv, &m_swizzleView );
			if ( FAILED( hr ) )
			{
				safeRelease( m_swizzleView ); safeRelease( m_swizzleTexture );
				m_swizzleFailed = true;
				std::fprintf( stderr, "[RW-GPU-SWIZZLE] shadow creation failed hr=0x%08lX %ux%u mips=%u layers=%u fmt=%d\n"
					, (unsigned long)hr, width, height, mips, layers, int( td.Format ) );
				get( m_device )->drainDebugMessages( "swizzle-shadow" );
				return m_shaderView;
			}
			if ( swz::logCount++ < 16u )
			{
				std::fprintf( stderr, "[RW-GPU-SWIZZLE] shadow vkFormat=%d %ux%u mips=%u layers=%u select=%u%u%u%u (0-3=RGBA,4=0,5=1)\n"
					, int( m_createInfo.format ), width, height, mips, layers
					, m_swizzleSelect[0], m_swizzleSelect[1], m_swizzleSelect[2], m_swizzleSelect[3] );
			}
		}

		// From here on the context is modified (and restored); tell the caller.
		if ( rendered ) *rendered = true;

		// Save the state the draws touch.
		ID3D11RenderTargetView * savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView * savedDsv{};
		ID3D11UnorderedAccessView * savedUavs[outputstate::UavSlots]{};
		outputstate::save( ctx, savedRtvs, &savedDsv, savedUavs );   // v13.43: UAVs too
		ID3D11BlendState * savedBs{}; FLOAT savedBf[4]{}; UINT savedMask{};
		ctx->OMGetBlendState( &savedBs, savedBf, &savedMask );
		ID3D11DepthStencilState * savedDs{}; UINT savedRef{};
		ctx->OMGetDepthStencilState( &savedDs, &savedRef );
		ID3D11RasterizerState * savedRs{};
		ctx->RSGetState( &savedRs );
		UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT savedVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		ctx->RSGetViewports( &vpCount, savedVps );
		ID3D11VertexShader * savedVs{}; ID3D11PixelShader * savedPs{}; ID3D11GeometryShader * savedGs{};
		ID3D11HullShader * savedHs{}; ID3D11DomainShader * savedDsh{};
		ctx->VSGetShader( &savedVs, nullptr, nullptr ); ctx->PSGetShader( &savedPs, nullptr, nullptr );
		ctx->GSGetShader( &savedGs, nullptr, nullptr ); ctx->HSGetShader( &savedHs, nullptr, nullptr );
		ctx->DSGetShader( &savedDsh, nullptr, nullptr );
		ID3D11InputLayout * savedLayout{}; ctx->IAGetInputLayout( &savedLayout );
		D3D11_PRIMITIVE_TOPOLOGY savedTopo{}; ctx->IAGetPrimitiveTopology( &savedTopo );
		ID3D11ShaderResourceView * savedSrv{}; ctx->PSGetShaderResources( 0u, 1u, &savedSrv );
		ID3D11Buffer * savedCb{}; ctx->PSGetConstantBuffers( 0u, 1u, &savedCb );

		uint32_t sel[4]{ m_swizzleSelect[0], m_swizzleSelect[1], m_swizzleSelect[2], m_swizzleSelect[3] };
		ctx->UpdateSubresource( shared.cb, 0u, nullptr, sel, 0u, 0u );
		ctx->OMSetBlendState( shared.bs, nullptr, 0xFFFFFFFFu );
		ctx->OMSetDepthStencilState( shared.ds, 0u );
		ctx->RSSetState( shared.rs );
		ctx->VSSetShader( shared.vs, nullptr, 0u ); ctx->PSSetShader( shared.ps, nullptr, 0u );
		ctx->GSSetShader( nullptr, nullptr, 0u ); ctx->HSSetShader( nullptr, nullptr, 0u ); ctx->DSSetShader( nullptr, nullptr, 0u );
		ctx->IASetInputLayout( nullptr );
		ctx->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
		ctx->PSSetConstantBuffers( 0u, 1u, &shared.cb );

		ID3D11Resource * srcRes{};
		m_shaderView->GetResource( &srcRes );
		UINT const srcFirstMip = is2DArray ? srcDesc.Texture2DArray.MostDetailedMip : srcDesc.Texture2D.MostDetailedMip;
		UINT const srcFirstLayer = is2DArray ? srcDesc.Texture2DArray.FirstArraySlice : 0u;
		bool ok = true;
		for ( UINT layer = 0u; layer < layers && ok; ++layer )
		{
			for ( UINT mip = 0u; mip < mips && ok; ++mip )
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
				sd.Format = srcDesc.Format;
				sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				sd.Texture2DArray.MostDetailedMip = srcFirstMip + mip; sd.Texture2DArray.MipLevels = 1u;
				sd.Texture2DArray.FirstArraySlice = srcFirstLayer + layer; sd.Texture2DArray.ArraySize = 1u;
				D3D11_RENDER_TARGET_VIEW_DESC rd{};
				rd.Format = swz::shadowFormat( srcDesc.Format );
				rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				rd.Texture2DArray.MipSlice = mip; rd.Texture2DArray.FirstArraySlice = layer; rd.Texture2DArray.ArraySize = 1u;
				ID3D11ShaderResourceView * srv{}; ID3D11RenderTargetView * rtv{};
				HRESULT hr = d3d->CreateShaderResourceView( srcRes, &sd, &srv );
				if ( SUCCEEDED( hr ) ) hr = d3d->CreateRenderTargetView( m_swizzleTexture, &rd, &rtv );
				if ( FAILED( hr ) )
				{
					ok = false;
					std::fprintf( stderr, "[RW-GPU-SWIZZLE] per-mip view failed hr=0x%08lX mip=%u layer=%u\n", (unsigned long)hr, mip, layer );
					get( m_device )->drainDebugMessages( "swizzle-views" );
				}
				else
				{
					ID3D11ShaderResourceView * nullSrv{};
					ctx->PSSetShaderResources( 0u, 1u, &nullSrv );
					ctx->OMSetRenderTargets( 1u, &rtv, nullptr );
					ctx->PSSetShaderResources( 0u, 1u, &srv );
					D3D11_VIEWPORT vp{ 0.0f, 0.0f, float( std::max( 1u, width >> mip ) ), float( std::max( 1u, height >> mip ) ), 0.0f, 1.0f };
					ctx->RSSetViewports( 1u, &vp );
					ctx->Draw( 3u, 0u );
				}
				safeRelease( srv ); safeRelease( rtv );
			}
		}
		safeRelease( srcRes );
		ID3D11ShaderResourceView * nullSrv{};
		ctx->PSSetShaderResources( 0u, 1u, &nullSrv );

		// Restore.
		outputstate::restore( ctx, savedRtvs, savedDsv, savedUavs );   // v13.43: re-binds graphics UAVs
		ctx->OMSetBlendState( savedBs, savedBf, savedMask );
		ctx->OMSetDepthStencilState( savedDs, savedRef );
		ctx->RSSetState( savedRs );
		if ( vpCount ) ctx->RSSetViewports( vpCount, savedVps );
		ctx->VSSetShader( savedVs, nullptr, 0u ); ctx->PSSetShader( savedPs, nullptr, 0u );
		ctx->GSSetShader( savedGs, nullptr, 0u ); ctx->HSSetShader( savedHs, nullptr, 0u ); ctx->DSSetShader( savedDsh, nullptr, 0u );
		ctx->IASetInputLayout( savedLayout ); ctx->IASetPrimitiveTopology( savedTopo );
		ctx->PSSetShaderResources( 0u, 1u, &savedSrv );
		ctx->PSSetConstantBuffers( 0u, 1u, &savedCb );
		for ( auto & r : savedRtvs ) safeRelease( r );
		safeRelease( savedDsv ); safeRelease( savedBs ); safeRelease( savedDs ); safeRelease( savedRs );
		safeRelease( savedVs ); safeRelease( savedPs ); safeRelease( savedGs ); safeRelease( savedHs ); safeRelease( savedDsh );
		safeRelease( savedLayout ); safeRelease( savedSrv ); safeRelease( savedCb );

		if ( !ok )
		{
			m_swizzleFailed = true;
			return m_shaderView;
		}
		m_swizzleVersion = version;
		return m_swizzleView;
	}
}
