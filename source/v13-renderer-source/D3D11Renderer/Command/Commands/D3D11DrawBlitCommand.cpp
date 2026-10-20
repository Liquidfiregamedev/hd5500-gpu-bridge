/*
This file belongs to Ashes.
See LICENSE file in root folder

v13.12 (HD5500 GPU bridge): draw-based vkCmdBlitImage. See the header.
*/
#include "Miscellaneous/D3D11OutputState.hpp"
#include "Command/Commands/D3D11DrawBlitCommand.hpp"

#include "Core/D3D11Device.hpp"
#include "Image/D3D11Image.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"

#include "ashesd3d11_api.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace ashes::D3D11_NAMESPACE
{
	//*********************************************************************************************
	// Per-device shared objects (shaders, states, constant buffer).
	//*********************************************************************************************

	struct DrawBlitShared
	{
		ID3D11Device * device{};
		ID3D11VertexShader * vs{};
		ID3D11PixelShader * ps{};
		ID3D11Buffer * cb{};
		ID3D11SamplerState * nearest{};
		ID3D11SamplerState * linear{};
		ID3D11RasterizerState * rasterizer{};
		ID3D11BlendState * blend{};
		ID3D11DepthStencilState * depth{};
		bool ok{};

		~DrawBlitShared()noexcept
		{
			safeRelease( depth );
			safeRelease( blend );
			safeRelease( rasterizer );
			safeRelease( linear );
			safeRelease( nearest );
			safeRelease( cb );
			safeRelease( ps );
			safeRelease( vs );
		}
	};

	namespace drawblit
	{
		static char const * const hlsl = R"(
cbuffer BlitCB : register(b0)
{
	float4 uvRect;   // origin.xy, extent.xy in normalised source coordinates
	float4 flip;     // x: 1 or -1, y: 1 or -1 (negative = mirrored), zw unused
};
Texture2D<float4> srcTex : register(t0);
SamplerState srcSampler : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vsMain( uint id : SV_VertexID )
{
	// Full-screen triangle; the viewport is the destination rectangle.
	float2 p = float2( ( id << 1 ) & 2, id & 2 );      // (0,0) (2,0) (0,2)
	VSOut o;
	o.pos = float4( p * float2( 2.0, -2.0 ) + float2( -1.0, 1.0 ), 0.0, 1.0 );
	float2 t = p;                                       // 0..2 across the triangle, 0..1 across the viewport
	t = lerp( t, 1.0 - t, step( flip.xy, 0.0 ) );       // mirror when flip < 0 (flip is +-1, so compares against 0)
	o.uv = uvRect.xy + t * uvRect.zw;
	return o;
}

float4 psMain( VSOut i ) : SV_Target
{
	return srcTex.SampleLevel( srcSampler, i.uv, 0.0 );
}
)";

		static ID3DBlob * compile( char const * entry, char const * profile )
		{
			ID3DBlob * code{};
			ID3DBlob * errors{};
			auto hr = D3DCompile( hlsl, std::strlen( hlsl ), "drawblit.hlsl", nullptr, nullptr, entry, profile
				, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors );
			if ( FAILED( hr ) )
			{
				std::fprintf( stderr, "[RW-GPU-DRAWBLIT] shader compile failed hr=0x%08lX: %s\n", (unsigned long)hr
					, errors ? static_cast< char const * >( errors->GetBufferPointer() ) : "" );
			}
			safeRelease( errors );
			return code;
		}

		static std::shared_ptr< DrawBlitShared > getShared( VkDevice vkDevice )
		{
			static std::mutex mutex;
			static std::map< ID3D11Device *, std::weak_ptr< DrawBlitShared > > cache;
			auto d3d = get( vkDevice )->getDevice();
			std::lock_guard< std::mutex > lock{ mutex };
			if ( auto it = cache.find( d3d ); it != cache.end() )
			{
				if ( auto shared = it->second.lock() ) return shared;
			}
			auto shared = std::make_shared< DrawBlitShared >();
			shared->device = d3d;
			auto vsCode = compile( "vsMain", "vs_4_0" );
			auto psCode = compile( "psMain", "ps_4_0" );
			HRESULT hr = ( vsCode && psCode ) ? S_OK : E_FAIL;
			if ( SUCCEEDED( hr ) ) hr = d3d->CreateVertexShader( vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &shared->vs );
			if ( SUCCEEDED( hr ) ) hr = d3d->CreatePixelShader( psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &shared->ps );
			safeRelease( vsCode );
			safeRelease( psCode );
			if ( SUCCEEDED( hr ) )
			{
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = 32u;
				bd.Usage = D3D11_USAGE_DYNAMIC;
				bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				hr = d3d->CreateBuffer( &bd, nullptr, &shared->cb );
			}
			for ( int i = 0; i < 2 && SUCCEEDED( hr ); ++i )
			{
				D3D11_SAMPLER_DESC sd{};
				sd.Filter = i ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
				sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				sd.MaxLOD = D3D11_FLOAT32_MAX;
				sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
				hr = d3d->CreateSamplerState( &sd, i ? &shared->linear : &shared->nearest );
			}
			if ( SUCCEEDED( hr ) )
			{
				D3D11_RASTERIZER_DESC rd{};
				rd.FillMode = D3D11_FILL_SOLID;
				rd.CullMode = D3D11_CULL_NONE;
				rd.DepthClipEnable = TRUE;
				hr = d3d->CreateRasterizerState( &rd, &shared->rasterizer );
			}
			if ( SUCCEEDED( hr ) )
			{
				D3D11_BLEND_DESC bd{};
				bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
				hr = d3d->CreateBlendState( &bd, &shared->blend );
			}
			if ( SUCCEEDED( hr ) )
			{
				D3D11_DEPTH_STENCIL_DESC dd{};
				dd.DepthEnable = FALSE;
				dd.StencilEnable = FALSE;
				hr = d3d->CreateDepthStencilState( &dd, &shared->depth );
			}
			shared->ok = SUCCEEDED( hr );
			if ( !shared->ok )
			{
				std::fprintf( stderr, "[RW-GPU-DRAWBLIT] shared object creation failed hr=0x%08lX\n", (unsigned long)hr );
			}
			cache[d3d] = shared;
			return shared;
		}

		// Typed view format for a (possibly typeless) resource format, preferring the
		// Vulkan format's own typed mapping when it is in the same cast family.
		static DXGI_FORMAT typedFor( DXGI_FORMAT resourceFormat, DXGI_FORMAT preferred )noexcept
		{
			if ( preferred != DXGI_FORMAT_UNKNOWN
				&& getDxgiFormatGroup( preferred ) == getDxgiFormatGroup( resourceFormat ) )
			{
				return preferred;
			}
			switch ( resourceFormat )
			{
			case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
			case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
			case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
			case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
			case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
			case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
			case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
			case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
			case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
			case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
			case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
			case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
			case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
			default: return resourceFormat;
			}
		}

		static bool getDesc( ID3D11Resource * res, D3D11_TEXTURE2D_DESC & desc )noexcept
		{
			ID3D11Texture2D * tex{};
			if ( !res || FAILED( res->QueryInterface( __uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &tex ) ) ) || !tex ) return false;
			tex->GetDesc( &desc );
			tex->Release();
			return true;
		}

		static bool formatSupports( ID3D11Device * d3d, DXGI_FORMAT format, UINT bits )noexcept
		{
			UINT support = 0u;
			return format != DXGI_FORMAT_UNKNOWN
				&& SUCCEEDED( d3d->CheckFormatSupport( format, &support ) )
				&& ( support & bits ) == bits;
		}

		static std::atomic< uint32_t > degradedCount{ 0u };
	}

	//*********************************************************************************************

	bool DrawBlitCommand::isSupported( VkDevice device
		, VkImage srcImage
		, VkImage dstImage )noexcept
	{
		auto src = get( srcImage );
		auto dst = get( dstImage );
		if ( src->getCreateInfo().imageType != VK_IMAGE_TYPE_2D || dst->getCreateInfo().imageType != VK_IMAGE_TYPE_2D ) return false;
		if ( src->getSamplesCount() != VK_SAMPLE_COUNT_1_BIT || dst->getSamplesCount() != VK_SAMPLE_COUNT_1_BIT ) return false;
		if ( isDepthOrStencilFormat( src->getFormat() ) || isDepthOrStencilFormat( dst->getFormat() ) ) return false;
		if ( isCompressedFormat( dst->getFormat() ) ) return false;
		D3D11_TEXTURE2D_DESC sd{}, dd{};
		if ( !drawblit::getDesc( src->getResource(), sd ) || !drawblit::getDesc( dst->getResource(), dd ) ) return false;
		// The destination must have been created as a render target (Image promotes
		// TRANSFER_DST images when the driver can RTV the format, v13.11).
		if ( !( dd.BindFlags & D3D11_BIND_RENDER_TARGET ) ) return false;
		if ( !( sd.BindFlags & D3D11_BIND_SHADER_RESOURCE ) ) return false;
		auto d3d = get( device )->getDevice();
		auto srvFmt = drawblit::typedFor( sd.Format, getSRVFormat( src->getFormat() ) );
		auto rtvFmt = drawblit::typedFor( dd.Format, getRTVFormat( dst->getFormat() ) );
		return drawblit::formatSupports( d3d, srvFmt, D3D11_FORMAT_SUPPORT_SHADER_SAMPLE )
			&& drawblit::formatSupports( d3d, rtvFmt, D3D11_FORMAT_SUPPORT_RENDER_TARGET );
	}

	DrawBlitCommand::DrawBlitCommand( VkDevice device
		, VkImage srcImage
		, VkImage dstImage
		, ArrayView< VkImageBlit const > const & regions
		, VkFilter filter )
		: CommandBase{ device }
		, m_shared{ drawblit::getShared( device ) }
		, m_ops{ std::make_shared< std::vector< Op > >() }
		, m_srcResource{ get( srcImage )->getResource() }
		, m_dstResource{ get( dstImage )->getResource() }
		, m_dstImage{ dstImage }
		, m_filter{ filter }
	{
		if ( !m_shared || !m_shared->ok ) throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Draw blit shared objects unavailable" };
		auto src = get( srcImage );
		auto dst = get( dstImage );
		auto d3d = get( device )->getDevice();
		D3D11_TEXTURE2D_DESC sd{}, dd{};
		drawblit::getDesc( m_srcResource, sd );
		drawblit::getDesc( m_dstResource, dd );
		auto const srvFmt = drawblit::typedFor( sd.Format, getSRVFormat( src->getFormat() ) );
		auto const rtvFmt = drawblit::typedFor( dd.Format, getRTVFormat( dst->getFormat() ) );
		bool const srcArray = sd.ArraySize > 1u;
		bool const dstArray = dd.ArraySize > 1u;

		for ( auto const & region : regions )
		{
			if ( region.srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT
				|| region.dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT
				|| region.srcOffsets[0].z != 0 || region.srcOffsets[1].z != 1
				|| region.dstOffsets[0].z != 0 || region.dstOffsets[1].z != 1
				|| region.srcSubresource.layerCount != region.dstSubresource.layerCount )
			{
				if ( drawblit::degradedCount++ < 32u )
				{
					std::fprintf( stderr, "[RW-GPU-DRAWBLIT] skipped region: non-colour / 3D / layer-count mismatch (src %d %d dst %d %d)\n"
						, region.srcSubresource.aspectMask, region.srcSubresource.layerCount, region.dstSubresource.aspectMask, region.dstSubresource.layerCount );
				}
				continue;
			}
			int const sx0 = region.srcOffsets[0].x, sy0 = region.srcOffsets[0].y, sx1 = region.srcOffsets[1].x, sy1 = region.srcOffsets[1].y;
			int const dx0 = region.dstOffsets[0].x, dy0 = region.dstOffsets[0].y, dx1 = region.dstOffsets[1].x, dy1 = region.dstOffsets[1].y;
			if ( sx0 == sx1 || sy0 == sy1 || dx0 == dx1 || dy0 == dy1 ) continue; // empty, nothing to do
			// Mirroring: a flipped source range or a flipped destination range mirrors
			// the sampling direction; both flipped cancel out.
			bool const flipX = ( sx1 < sx0 ) != ( dx1 < dx0 );
			bool const flipY = ( sy1 < sy0 ) != ( dy1 < dy0 );
			int const sl = std::min( sx0, sx1 ), st = std::min( sy0, sy1 ), sr = std::max( sx0, sx1 ), sb = std::max( sy0, sy1 );
			int const dl = std::min( dx0, dx1 ), dt = std::min( dy0, dy1 ), dr = std::max( dx0, dx1 ), db = std::max( dy0, dy1 );
			auto const srcMip = region.srcSubresource.mipLevel;
			auto const dstMip = region.dstSubresource.mipLevel;
			auto const srcW = std::max( 1u, sd.Width >> srcMip ), srcH = std::max( 1u, sd.Height >> srcMip );
			auto const dstW = std::max( 1u, dd.Width >> dstMip ), dstH = std::max( 1u, dd.Height >> dstMip );

			for ( uint32_t layer = 0u; layer < region.srcSubresource.layerCount; ++layer )
			{
				auto const srcLayer = region.srcSubresource.baseArrayLayer + layer;
				auto const dstLayer = region.dstSubresource.baseArrayLayer + layer;
				// Same size, same cast family, different subresource: a plain copy is
				// exact and cheaper than a draw (also the only route when sampling the
				// source is not possible).
				bool const sameSize = ( sr - sl ) == ( dr - dl ) && ( sb - st ) == ( db - dt ) && !flipX && !flipY;
				bool const sameSub = m_srcResource == m_dstResource && srcMip == dstMip && srcLayer == dstLayer;
				if ( sameSize && !sameSub
					&& getDxgiFormatGroup( sd.Format ) == getDxgiFormatGroup( dd.Format )
					&& sl >= 0 && st >= 0 && dl >= 0 && dt >= 0
					&& uint32_t( sr ) <= srcW && uint32_t( sb ) <= srcH && uint32_t( dr ) <= dstW && uint32_t( db ) <= dstH )
				{
					Copy copy{};
					copy.srcSubresource = D3D11CalcSubresource( srcMip, srcLayer, sd.MipLevels );
					copy.dstSubresource = D3D11CalcSubresource( dstMip, dstLayer, dd.MipLevels );
					copy.srcBox = D3D11_BOX{ UINT( sl ), UINT( st ), 0u, UINT( sr ), UINT( sb ), 1u };
					copy.dstX = UINT( dl ); copy.dstY = UINT( dt );
					m_copies.push_back( copy );
					continue;
				}
				if ( sameSub )
				{
					if ( drawblit::degradedCount++ < 32u ) std::fprintf( stderr, "[RW-GPU-DRAWBLIT] skipped region: source and destination are the same subresource\n" );
					continue;
				}

				Op op{};
				D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
				svd.Format = srvFmt;
				if ( srcArray )
				{
					svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
					svd.Texture2DArray.MostDetailedMip = srcMip; svd.Texture2DArray.MipLevels = 1u;
					svd.Texture2DArray.FirstArraySlice = srcLayer; svd.Texture2DArray.ArraySize = 1u;
				}
				else
				{
					svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					svd.Texture2D.MostDetailedMip = srcMip; svd.Texture2D.MipLevels = 1u;
				}
				D3D11_RENDER_TARGET_VIEW_DESC rvd{};
				rvd.Format = rtvFmt;
				if ( dstArray )
				{
					rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
					rvd.Texture2DArray.MipSlice = dstMip; rvd.Texture2DArray.FirstArraySlice = dstLayer; rvd.Texture2DArray.ArraySize = 1u;
				}
				else
				{
					rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
					rvd.Texture2D.MipSlice = dstMip;
				}
				auto hr = d3d->CreateShaderResourceView( m_srcResource, &svd, &op.srv );
				if ( SUCCEEDED( hr ) ) hr = d3d->CreateRenderTargetView( m_dstResource, &rvd, &op.rtv );
				if ( FAILED( hr ) )
				{
					safeRelease( op.srv );
					safeRelease( op.rtv );
					get( device )->drainDebugMessages( "drawblit-views" );
					if ( drawblit::degradedCount++ < 32u )
					{
						std::fprintf( stderr, "[RW-GPU-DRAWBLIT] view creation failed hr=0x%08lX srvFmt=%d rtvFmt=%d srcFmt=%d dstFmt=%d; region skipped\n"
							, (unsigned long)hr, int( srvFmt ), int( rtvFmt ), int( src->getFormat() ), int( dst->getFormat() ) );
					}
					continue;
				}
				op.viewport = D3D11_VIEWPORT{ float( dl ), float( dt ), float( dr - dl ), float( db - dt ), 0.0f, 1.0f };
				op.cb[0] = float( sl ) / float( srcW );
				op.cb[1] = float( st ) / float( srcH );
				op.cb[2] = float( sr - sl ) / float( srcW );
				op.cb[3] = float( sb - st ) / float( srcH );
				op.cb[4] = flipX ? -1.0f : 1.0f;
				op.cb[5] = flipY ? -1.0f : 1.0f;
				m_ops->push_back( op );
			}
		}
		framediag::onBlitRecorded( m_srcResource, m_dstResource, uint32_t( m_ops->size() + m_copies.size() ), int( filter ) );
	}

	DrawBlitCommand::~DrawBlitCommand()noexcept
	{
		if ( m_ops && m_ops.use_count() == 1 )
		{
			for ( auto & op : *m_ops )
			{
				safeRelease( op.srv );
				safeRelease( op.rtv );
			}
		}
	}

	void DrawBlitCommand::apply( Context & context )const
	{
		get( m_dstImage )->bumpContentVersion(); // v13.14
		auto ctx = context.context.operator->();
		for ( auto const & copy : m_copies )
		{
			ctx->CopySubresourceRegion( m_dstResource, copy.dstSubresource, copy.dstX, copy.dstY, 0u
				, m_srcResource, copy.srcSubresource, &copy.srcBox );
		}
		if ( m_ops->empty() ) return;

		// Save every piece of state the draw touches. Vulkan keeps pipeline and
		// descriptor bindings alive across a blit, so later draws may rely on them.
		ID3D11RenderTargetView * savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView * savedDsv{};
		ID3D11UnorderedAccessView * savedUavs[outputstate::UavSlots]{};
		outputstate::save( ctx, savedRtvs, &savedDsv, savedUavs );   // v13.43: UAVs too
		ID3D11BlendState * savedBlend{}; FLOAT savedBlendFactor[4]{}; UINT savedSampleMask{};
		ctx->OMGetBlendState( &savedBlend, savedBlendFactor, &savedSampleMask );
		ID3D11DepthStencilState * savedDepth{}; UINT savedStencilRef{};
		ctx->OMGetDepthStencilState( &savedDepth, &savedStencilRef );
		ID3D11RasterizerState * savedRs{};
		ctx->RSGetState( &savedRs );
		UINT savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		ctx->RSGetViewports( &savedViewportCount, savedViewports );
		UINT savedScissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_RECT savedScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		ctx->RSGetScissorRects( &savedScissorCount, savedScissors );
		ID3D11VertexShader * savedVs{}; ID3D11PixelShader * savedPs{}; ID3D11GeometryShader * savedGs{};
		ID3D11HullShader * savedHs{}; ID3D11DomainShader * savedDs{};
		ctx->VSGetShader( &savedVs, nullptr, nullptr );
		ctx->PSGetShader( &savedPs, nullptr, nullptr );
		ctx->GSGetShader( &savedGs, nullptr, nullptr );
		ctx->HSGetShader( &savedHs, nullptr, nullptr );
		ctx->DSGetShader( &savedDs, nullptr, nullptr );
		ID3D11InputLayout * savedLayout{};
		ctx->IAGetInputLayout( &savedLayout );
		D3D11_PRIMITIVE_TOPOLOGY savedTopology{};
		ctx->IAGetPrimitiveTopology( &savedTopology );
		ID3D11ShaderResourceView * savedSrv{};
		ctx->PSGetShaderResources( 0u, 1u, &savedSrv );
		ID3D11SamplerState * savedSampler{};
		ctx->PSGetSamplers( 0u, 1u, &savedSampler );
		ID3D11Buffer * savedCb{};
		ctx->VSGetConstantBuffers( 0u, 1u, &savedCb );

		// Common state.
		ctx->OMSetBlendState( m_shared->blend, nullptr, 0xFFFFFFFFu );
		ctx->OMSetDepthStencilState( m_shared->depth, 0u );
		ctx->RSSetState( m_shared->rasterizer );
		ctx->VSSetShader( m_shared->vs, nullptr, 0u );
		ctx->PSSetShader( m_shared->ps, nullptr, 0u );
		ctx->GSSetShader( nullptr, nullptr, 0u );
		ctx->HSSetShader( nullptr, nullptr, 0u );
		ctx->DSSetShader( nullptr, nullptr, 0u );
		ctx->IASetInputLayout( nullptr );
		ctx->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
		ID3D11SamplerState * sampler = m_filter == VK_FILTER_NEAREST ? m_shared->nearest : m_shared->linear;
		ctx->PSSetSamplers( 0u, 1u, &sampler );
		ctx->VSSetConstantBuffers( 0u, 1u, &m_shared->cb );

		for ( auto const & op : *m_ops )
		{
			// Unbind the SRV slot first so an in-place mip chain never has the destination
			// subresource's resource bound as SRV while its RTV is set (D3D11 hazard rule).
			ID3D11ShaderResourceView * nullSrv{};
			ctx->PSSetShaderResources( 0u, 1u, &nullSrv );
			ctx->OMSetRenderTargets( 1u, &op.rtv, nullptr );
			ctx->PSSetShaderResources( 0u, 1u, &op.srv );
			ctx->RSSetViewports( 1u, &op.viewport );
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if ( SUCCEEDED( ctx->Map( m_shared->cb, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped ) ) )
			{
				std::memcpy( mapped.pData, op.cb, sizeof( op.cb ) );
				ctx->Unmap( m_shared->cb, 0u );
			}
			ctx->Draw( 3u, 0u );
		}
		ID3D11ShaderResourceView * nullSrv{};
		ctx->PSSetShaderResources( 0u, 1u, &nullSrv );
		ID3D11RenderTargetView * nullRtv{};
		ctx->OMSetRenderTargets( 1u, &nullRtv, nullptr );

		// Restore.
		outputstate::restore( ctx, savedRtvs, savedDsv, savedUavs );   // v13.43: re-binds graphics UAVs
		ctx->OMSetBlendState( savedBlend, savedBlendFactor, savedSampleMask );
		ctx->OMSetDepthStencilState( savedDepth, savedStencilRef );
		ctx->RSSetState( savedRs );
		if ( savedViewportCount ) ctx->RSSetViewports( savedViewportCount, savedViewports );
		if ( savedScissorCount ) ctx->RSSetScissorRects( savedScissorCount, savedScissors );
		ctx->VSSetShader( savedVs, nullptr, 0u );
		ctx->PSSetShader( savedPs, nullptr, 0u );
		ctx->GSSetShader( savedGs, nullptr, 0u );
		ctx->HSSetShader( savedHs, nullptr, 0u );
		ctx->DSSetShader( savedDs, nullptr, 0u );
		ctx->IASetInputLayout( savedLayout );
		ctx->IASetPrimitiveTopology( savedTopology );
		ctx->PSSetShaderResources( 0u, 1u, &savedSrv );
		ctx->PSSetSamplers( 0u, 1u, &savedSampler );
		ctx->VSSetConstantBuffers( 0u, 1u, &savedCb );
		for ( auto & rtv : savedRtvs ) safeRelease( rtv );
		safeRelease( savedDsv ); safeRelease( savedBlend ); safeRelease( savedDepth ); safeRelease( savedRs );
		safeRelease( savedVs ); safeRelease( savedPs ); safeRelease( savedGs ); safeRelease( savedHs ); safeRelease( savedDs );
		safeRelease( savedLayout ); safeRelease( savedSrv ); safeRelease( savedSampler ); safeRelease( savedCb );
		// The bridge's state cache mirrors what it last set; it must re-apply after us.
		context.stateCache.invalidate();
	}

	CommandPtr DrawBlitCommand::clone()const
	{
		return std::make_unique< DrawBlitCommand >( *this );
	}
}
