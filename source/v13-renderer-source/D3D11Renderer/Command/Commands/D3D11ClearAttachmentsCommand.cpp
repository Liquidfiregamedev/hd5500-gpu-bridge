/*
This file belongs to Ashes.
See LICENSE file in root folder.

v13.21 (HD5500 GPU bridge): rectangle-aware vkCmdClearAttachments.
Vulkan clears only the given rectangles. D3D11's ClearRenderTargetView and
ClearDepthStencilView always clear the WHOLE view and ignore the scissor. Waylith
fills its 2048x2048 pawn atlas cell by cell and clears each cell before drawing a
pawn into it, so every cell clear erased all other pawns (map pawns became empty
silhouettes while the separate portrait textures looked fine).
  - whole-target rectangle  -> ClearRenderTargetView / ClearDepthStencilView (fast)
  - partial rectangle       -> a tiny draw restricted to the rectangle (colour: writes
                               only that target; depth/stencil: depth ALWAYS + stencil
                               REPLACE). Works on every D3D11 driver and in Wine.
  - RW_GPU_RECTCLEAR=clearview -> colour rectangles via ID3D11DeviceContext1::ClearView
                               instead (D3D11.1; cheaper, but Wine only stubs it, so it
                               is opt-in until the laptop self-test proves it)
Everything is decided from the rectangle and the interfaces the driver offers,
nothing GPU-specific.
*/
#include "Miscellaneous/D3D11OutputState.hpp"
#include "Command/Commands/D3D11ClearAttachmentsCommand.hpp"
#include "Command/Commands/D3D11RectClear.hpp"

#include "Core/D3D11Device.hpp"
#include "Image/D3D11ImageView.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"
#include "RenderPass/D3D11FrameBuffer.hpp"

#include "ashesd3d11_api.hpp"

#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace ashes::D3D11_NAMESPACE
{
	struct RectClearShared
	{
		ID3D11VertexShader * vs{};
		ID3D11PixelShader * ps{};
		ID3D11Buffer * cb{};
		ID3D11RasterizerState * rasterizer{};
		bool ok{};

		~RectClearShared()noexcept
		{
			safeRelease( rasterizer );
			safeRelease( cb );
			safeRelease( ps );
			safeRelease( vs );
		}
	};

	namespace clratt
	{
		static std::atomic< uint32_t > logCount{ 0u };

		static bool useClearView()
		{
			static bool const value = []()
			{
				char buffer[32]{};
				auto len = GetEnvironmentVariableA( "RW_GPU_RECTCLEAR", buffer, sizeof( buffer ) );
				bool on = len > 0 && len < sizeof( buffer ) && std::strcmp( buffer, "clearview" ) == 0;
				std::fprintf( stderr, "[RW-GPU-CLEAR] rectangle clears use %s\n", on ? "ClearView" : "draws" );
				return on;
			}();
			return value;
		}

		static char const * const hlsl = R"(
cbuffer ClearCB : register(b0)
{
	float4 colour;
	float4 depth;    // x: depth value
};
float4 vsMain( uint id : SV_VertexID ) : SV_Position
{
	float2 p = float2( ( id << 1 ) & 2, id & 2 );
	return float4( p * float2( 2.0, -2.0 ) + float2( -1.0, 1.0 ), depth.x, 1.0 );
}
float4 psMain() : SV_Target0 { return colour; }
)";

		static ID3DBlob * compile( char const * entry, char const * profile )
		{
			ID3DBlob * code{};
			ID3DBlob * errors{};
			auto hr = D3DCompile( hlsl, std::strlen( hlsl ), "rectclear.hlsl", nullptr, nullptr, entry, profile
				, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors );
			if ( FAILED( hr ) )
			{
				std::fprintf( stderr, "[RW-GPU-CLEAR] rect-clear shader compile failed hr=0x%08lX: %s\n", (unsigned long)hr
					, errors ? static_cast< char const * >( errors->GetBufferPointer() ) : "" );
			}
			safeRelease( errors );
			return code;
		}

		static std::shared_ptr< RectClearShared > getShared( ID3D11Device * d3d )
		{
			static std::mutex mutex;
			static std::map< ID3D11Device *, std::shared_ptr< RectClearShared > > cache;
			std::lock_guard< std::mutex > lock{ mutex };
			if ( auto it = cache.find( d3d ); it != cache.end() ) return it->second;
			auto shared = std::make_shared< RectClearShared >();
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
			if ( SUCCEEDED( hr ) )
			{
				D3D11_RASTERIZER_DESC rd{};
				rd.FillMode = D3D11_FILL_SOLID;
				rd.CullMode = D3D11_CULL_NONE;
				rd.DepthClipEnable = FALSE;     // depth comes from the shader, never clip it away
				rd.ScissorEnable = FALSE;
				hr = d3d->CreateRasterizerState( &rd, &shared->rasterizer );
			}
			shared->ok = SUCCEEDED( hr );
			if ( !shared->ok )
			{
				std::fprintf( stderr, "[RW-GPU-CLEAR] rect-clear objects failed hr=0x%08lX\n", (unsigned long)hr );
			}
			cache[d3d] = shared;
			return shared;
		}

		static void wholeClear( Context const & context, ClearAttachmentView const & clearAttach )
		{
			if ( ashes::checkFlag( clearAttach.clear.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT ) )
			{
				auto & colour = clearAttach.clear.clearValue.color;
				framediag::onClearView( clearAttach.view, colour.float32, "vkCmdClearAttachments" );
				context.context->ClearRenderTargetView( static_cast< ID3D11RenderTargetView * >( clearAttach.view )
					, colour.float32 );
			}
			else
			{
				auto & depthStencil = clearAttach.clear.clearValue.depthStencil;
				UINT flags = ( ashes::checkFlag( clearAttach.clear.aspectMask, VK_IMAGE_ASPECT_DEPTH_BIT ) ? D3D11_CLEAR_DEPTH : 0u )
					| ( ashes::checkFlag( clearAttach.clear.aspectMask, VK_IMAGE_ASPECT_STENCIL_BIT ) ? D3D11_CLEAR_STENCIL : 0u );
				context.context->ClearDepthStencilView( static_cast< ID3D11DepthStencilView * >( clearAttach.view )
					, flags, depthStencil.depth, UINT8( depthStencil.stencil ) );
			}
		}

		// Draw-based clear of one rectangle: colour into one RTV, or depth/stencil into one DSV.
		static bool drawClear( Context & context, RectClearShared const & shared
			, ClearAttachmentView const & clearAttach, D3D11_RECT const & rect )
		{
			auto ctx = context.context.operator->();
			ID3D11Device * d3d{};
			ctx->GetDevice( &d3d );
			bool const isColour = ashes::checkFlag( clearAttach.clear.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT );
			bool const doDepth = ashes::checkFlag( clearAttach.clear.aspectMask, VK_IMAGE_ASPECT_DEPTH_BIT );
			bool const doStencil = ashes::checkFlag( clearAttach.clear.aspectMask, VK_IMAGE_ASPECT_STENCIL_BIT );

			// Per-call states are small and rare (cell clears); created on demand.
			ID3D11BlendState * blend{};
			ID3D11DepthStencilState * depth{};
			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].RenderTargetWriteMask = isColour ? D3D11_COLOR_WRITE_ENABLE_ALL : 0u;
			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = ( !isColour && doDepth ) ? TRUE : FALSE;
			dd.DepthWriteMask = ( !isColour && doDepth ) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
			dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
			dd.StencilEnable = ( !isColour && doStencil ) ? TRUE : FALSE;
			dd.StencilReadMask = 0xFF;
			dd.StencilWriteMask = 0xFF;
			dd.FrontFace = { D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
			dd.BackFace = dd.FrontFace;
			HRESULT hr = d3d->CreateBlendState( &bd, &blend );
			if ( SUCCEEDED( hr ) ) hr = d3d->CreateDepthStencilState( &dd, &depth );
			safeRelease( d3d );
			if ( FAILED( hr ) )
			{
				safeRelease( blend ); safeRelease( depth );
				return false;
			}

			// Save.
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
			ID3D11Buffer * savedVsCb{}; ID3D11Buffer * savedPsCb{};
			ctx->VSGetConstantBuffers( 0u, 1u, &savedVsCb );
			ctx->PSGetConstantBuffers( 0u, 1u, &savedPsCb );

			// Draw.
			float cbData[8]{};
			if ( isColour ) std::memcpy( cbData, clearAttach.clear.clearValue.color.float32, 16u );
			cbData[4] = clearAttach.clear.clearValue.depthStencil.depth;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if ( SUCCEEDED( ctx->Map( shared.cb, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped ) ) )
			{
				std::memcpy( mapped.pData, cbData, sizeof( cbData ) );
				ctx->Unmap( shared.cb, 0u );
			}
			if ( isColour )
			{
				auto rtv = static_cast< ID3D11RenderTargetView * >( clearAttach.view );
				ctx->OMSetRenderTargets( 1u, &rtv, nullptr );
			}
			else
			{
				ctx->OMSetRenderTargets( 0u, nullptr, static_cast< ID3D11DepthStencilView * >( clearAttach.view ) );
			}
			ctx->OMSetBlendState( blend, nullptr, 0xFFFFFFFFu );
			ctx->OMSetDepthStencilState( depth, UINT( clearAttach.clear.clearValue.depthStencil.stencil & 0xFFu ) );
			ctx->RSSetState( shared.rasterizer );
			D3D11_VIEWPORT vp{ float( rect.left ), float( rect.top ), float( rect.right - rect.left ), float( rect.bottom - rect.top ), 0.0f, 1.0f };
			ctx->RSSetViewports( 1u, &vp );
			ctx->VSSetShader( shared.vs, nullptr, 0u );
			ctx->PSSetShader( isColour ? shared.ps : nullptr, nullptr, 0u );
			ctx->GSSetShader( nullptr, nullptr, 0u );
			ctx->HSSetShader( nullptr, nullptr, 0u );
			ctx->DSSetShader( nullptr, nullptr, 0u );
			ctx->IASetInputLayout( nullptr );
			ctx->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
			ctx->VSSetConstantBuffers( 0u, 1u, &shared.cb );
			ctx->PSSetConstantBuffers( 0u, 1u, &shared.cb );
			ctx->Draw( 3u, 0u );

			// Restore.
			outputstate::restore( ctx, savedRtvs, savedDsv, savedUavs );   // v13.43: re-binds graphics UAVs
			ctx->OMSetBlendState( savedBlend, savedBlendFactor, savedSampleMask );
			ctx->OMSetDepthStencilState( savedDepth, savedStencilRef );
			ctx->RSSetState( savedRs );
			if ( savedViewportCount ) ctx->RSSetViewports( savedViewportCount, savedViewports );
			ctx->VSSetShader( savedVs, nullptr, 0u );
			ctx->PSSetShader( savedPs, nullptr, 0u );
			ctx->GSSetShader( savedGs, nullptr, 0u );
			ctx->HSSetShader( savedHs, nullptr, 0u );
			ctx->DSSetShader( savedDs, nullptr, 0u );
			ctx->IASetInputLayout( savedLayout );
			ctx->IASetPrimitiveTopology( savedTopology );
			ctx->VSSetConstantBuffers( 0u, 1u, &savedVsCb );
			ctx->PSSetConstantBuffers( 0u, 1u, &savedPsCb );
			for ( auto & rtv : savedRtvs ) safeRelease( rtv );
			safeRelease( savedDsv ); safeRelease( savedBlend ); safeRelease( savedDepth ); safeRelease( savedRs );
			safeRelease( savedVs ); safeRelease( savedPs ); safeRelease( savedGs ); safeRelease( savedHs ); safeRelease( savedDs );
			safeRelease( savedLayout ); safeRelease( savedVsCb ); safeRelease( savedPsCb );
			safeRelease( blend ); safeRelease( depth );
			context.stateCache.invalidate();
			return true;
		}
	}

	ClearAttachmentsCommand::ClearAttachmentsCommand( VkDevice device
		, VkSubpassDescription const & subpass
		, VkFramebuffer framebuffer
		, ArrayView< VkClearAttachment const > const & clearAttaches
		, ArrayView< VkClearRect const > const & clearRects )
		: CommandBase{ device }
		, m_clearRects{ clearRects.begin(), clearRects.end() }
		, m_fbExtent{ get( framebuffer )->getDimensions() }
		, m_vkDevice{ device }
	{
		for ( auto & attach : clearAttaches )
		{
			if ( checkFlag( attach.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT ) )
			{
				if ( attach.colorAttachment >= subpass.colorAttachmentCount ) continue;
				auto ref = subpass.pColorAttachments[attach.colorAttachment];
				if ( ref.attachment == VK_ATTACHMENT_UNUSED ) continue;
				m_clearViews.emplace_back( attach, get( framebuffer )->getAllViews()[ref.attachment]->view );
			}
			else if ( attach.aspectMask )
			{
				// v13.8: a depth/stencil clear on a framebuffer without a depth view is a no-op.
				auto dsView = get( framebuffer )->getDSView();
				if ( !dsView || !dsView->view ) continue;
				m_clearViews.emplace_back( attach, dsView->view );
			}
		}
	}

	bool clearViewRect( Context & context
		, VkDevice device
		, ID3D11View * view
		, VkImageAspectFlags aspects
		, VkClearValue const & value
		, D3D11_RECT const & r
		, VkExtent2D const & extent )
	{
		if ( !view || !aspects ) return false;
		VkClearAttachment attach{ aspects, 0u, value };
		ClearAttachmentView clearAttach{ attach, view };
		bool const isColour = checkFlag( aspects, VK_IMAGE_ASPECT_COLOR_BIT );
		bool const whole = r.left <= 0 && r.top <= 0
			&& r.right >= LONG( extent.width ) && r.bottom >= LONG( extent.height );
		if ( whole )
		{
			clratt::wholeClear( context, clearAttach );
			return true;
		}
		if ( r.right <= r.left || r.bottom <= r.top ) return true;
		if ( isColour && context.context1 && clratt::useClearView() )
		{
			auto & colour = value.color;
			framediag::onClearView( view, colour.float32, "rect-clear" );
			context.context1->ClearView( view, colour.float32, &r, 1u );
			return true;
		}
		auto shared = clratt::getShared( get( device )->getDevice() );
		bool done = shared && shared->ok && clratt::drawClear( context, *shared, clearAttach, r );
		if ( !done )
		{
			if ( clratt::logCount++ < 16u ) std::fprintf( stderr, "[RW-GPU-CLEAR] rectangle clear unavailable, clearing whole view\n" );
			clratt::wholeClear( context, clearAttach );
		}
		else if ( clratt::logCount++ < 4u )
		{
			std::fprintf( stderr, "[RW-GPU-CLEAR] rectangle %s clear by draw %ldx%ld at %ld,%ld\n"
				, isColour ? "colour" : "depth/stencil", r.right - r.left, r.bottom - r.top, r.left, r.top );
		}
		return true;
	}

	void ClearAttachmentsCommand::apply( Context & context )const
	{
		for ( auto & clearAttach : m_clearViews )
		{
			for ( auto & rect : m_clearRects )
			{
				D3D11_RECT r
				{
					LONG( rect.rect.offset.x ),
					LONG( rect.rect.offset.y ),
					LONG( rect.rect.offset.x ) + LONG( rect.rect.extent.width ),
					LONG( rect.rect.offset.y ) + LONG( rect.rect.extent.height ),
				};
				clearViewRect( context, m_vkDevice, clearAttach.view, clearAttach.clear.aspectMask
					, clearAttach.clear.clearValue, r, m_fbExtent );
			}
		}
	}

	CommandPtr ClearAttachmentsCommand::clone()const
	{
		return std::make_unique< ClearAttachmentsCommand >( *this );
	}
}
