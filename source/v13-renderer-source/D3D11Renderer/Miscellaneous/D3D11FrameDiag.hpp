/*
v13.4 frame-composition diagnostics for the HD5500 bridge (diagnostic only).

Answers "what lands in the swapchain image?" for a bounded set of frames:
  - every draw / render pass / clear / copy that targets a swapchain image, with the
    full D3D11 pipeline state at draw time (RTV, viewport, scissor, raster, blend,
    depth-stencil, shaders, pixel-shader SRVs);
  - a compact list of the first off-screen draws and passes;
  - per-frame totals;
  - on dump frames: full-frame statistics and a downscaled BMP of the swapchain image,
    of the textures sampled by the last draw into the swapchain, and of the last
    off-screen render target.

Enabled only with RW_HD5500_FRAME_DIAG=1. BMPs are written only when
RW_HD5500_FRAME_DUMP_DIR names a directory. Nothing here changes rendering:
the only device work is CopySubresourceRegion into private staging textures on dump frames.

RW_HD5500_SWAPCHAIN_IMAGES=1..3 (default 1) is read here too; it is a separate,
behaviour-changing A/B switch used by the surface caps and the swapchain.
*/
#pragma once

#include "Miscellaneous/D3D11FrameStats.hpp"
#include <d3d11.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ashes::D3D11_NAMESPACE::framediag
{
	inline bool readFlag( char const * name )
	{
		char value[ 4u ]{};
		auto const length = ::GetEnvironmentVariableA( name, value, DWORD( sizeof( value ) ) );
		return length == 1u && value[ 0 ] == '1';
	}

	inline bool readFlagZero( char const * name )   // v13.73: variable set to exactly "0"
	{
		char value[ 4u ]{};
		auto const length = ::GetEnvironmentVariableA( name, value, DWORD( sizeof( value ) ) );
		return length == 1u && value[ 0 ] == '0';
	}

	inline bool enabled()
	{
		static bool const result = readFlag( "RW_HD5500_FRAME_DIAG" );
		return result;
	}

	inline std::string const & dumpDir()
	{
		static std::string const result = []()
		{
			char buffer[ 1024u ]{};
			auto const length = ::GetEnvironmentVariableA( "RW_HD5500_FRAME_DUMP_DIR", buffer, DWORD( sizeof( buffer ) ) );
			return ( length > 0u && length < sizeof( buffer ) )
				? std::string( buffer, length )
				: std::string{};
		}();
		return result;
	}

	// Number of images a swapchain may expose (A/B switch). Default 1 = v13.3 behaviour.
	inline uint32_t swapchainImageSetting()
	{
		static uint32_t const result = []()
		{
			char value[ 4u ]{};
			auto const length = ::GetEnvironmentVariableA( "RW_HD5500_SWAPCHAIN_IMAGES", value, DWORD( sizeof( value ) ) );
			if ( length == 1u && value[ 0 ] >= '1' && value[ 0 ] <= '3' )
			{
				return uint32_t( value[ 0 ] - '0' );
			}
			return 1u;
		}();
		return result;
	}

	inline void emit( std::string const & line )
	{
		// One complete write per line so std::clog traffic cannot split it.
		std::string text = line;
		text += '\n';
		std::fwrite( text.data(), 1u, text.size(), stderr );
		std::fflush( stderr );
	}

	template< typename ... Args >
	inline std::string fmt( char const * format, Args ... args )
	{
		char buffer[ 1536u ];
		auto const written = std::snprintf( buffer, sizeof( buffer ), format, args ... );
		if ( written < 0 )
		{
			return {};
		}
		return std::string( buffer, size_t( std::min( written, int( sizeof( buffer ) - 1 ) ) ) );
	}

	template< typename T >
	inline void release( T *& object )
	{
		if ( object )
		{
			object->Release();
			object = nullptr;
		}
	}

	struct Target
	{
		ID3D11Resource * resource{};
		UINT subresource{};
		DXGI_FORMAT viewFormat{ DXGI_FORMAT_UNKNOWN };

		void reset()
		{
			release( resource );
			subresource = 0u;
			viewFormat = DXGI_FORMAT_UNKNOWN;
		}

		void set( ID3D11Resource * res, UINT sub, DXGI_FORMAT format )
		{
			if ( res )
			{
				res->AddRef();
			}
			reset();
			resource = res;
			subresource = sub;
			viewFormat = format;
		}
	};

	struct State
	{
		std::mutex mutex;
		std::vector< ID3D11Resource * > swapImages; // not AddRef'd; registered by the swapchain
		uint64_t frame{ 1u };
		bool trace{ true };
		std::atomic< bool > traceHint{ true };     // v13.73: lock-free copy of trace (per-draw early out)
		bool dump{ false };
		bool dumpLight{ false };   // v13.70: timed dump = swapchain picture only
		uint32_t draws{};
		uint32_t swapDraws{};
		uint32_t otherDraws{};
		uint32_t swapPasses{};
		uint32_t otherPasses{};
		uint32_t swapClears{};
		uint32_t swapCopies{};
		uint32_t swapLines{};
		uint32_t otherLines{};
		uint32_t passLines{};
		uint32_t blitRecordLines{};
		uint32_t dataLines{};
		uint32_t drawListLines{};
		uint32_t swapTexLines{};
		uint32_t atlasDrawLines{};
		uint32_t instUavLines{};   // v13.46
		std::string atlasKeys;
		// v13.30: set when an atlas draw was described; the draw command then compares
		// the GPU vertex/index bytes with the CPU shadow (stale-upload detector).
		bool compareRequested{};
		std::atomic< bool > compareHint{};        // v13.73: lock-free copy of compareRequested
		uint32_t compareLines{};
		Target lastSwapSrv[ 2u ];
		Target lastOther;
		// v13.18: every distinct offscreen render target of a dump frame (world HDR
		// target, pawn atlas, ...), not only the last one.
		Target otherTargets[ 8u ];
		uint32_t otherTargetCount{};
		// v13.22: render targets that are SAMPLED by this frame's draws (e.g. the pawn
		// atlas, which is only rendered when it changes but sampled every frame).
		Target sampledTargets[ 20u ];
		uint32_t sampledTargetCount{};
		// v13.18: also trace/dump every TimedIntervalMs, so long loads cannot skip
		// past the fixed frame numbers before the map appears.
		uint64_t lastTimedTraceMs{};
	};

	inline State & state()
	{
		static State result;
		return result;
	}

	inline bool hintsOn()
	{
		static bool const result = !readFlagZero( "RW_HD5500_DIAG_HINTS" );
		return result;
	}

	inline bool takeCompareRequest()
	{
		if ( !enabled() ) return false;   // v13.51: no mutex per draw in normal play
		auto & s = state();
		if ( hintsOn() && !s.compareHint.load( std::memory_order_relaxed ) ) return false;   // v13.73
		std::lock_guard< std::mutex > lock{ s.mutex };
		bool const result = s.compareRequested;
		s.compareRequested = false;
		s.compareHint.store( false, std::memory_order_relaxed );
		return result;
	}

	inline bool isTraceFrame( uint64_t frame )
	{
		static uint64_t const frames[]{ 1, 2, 3, 5, 10, 20, 30, 60, 100, 150, 200, 300, 450, 600, 900, 1200, 1800, 2400, 3600 };
		return std::find( std::begin( frames ), std::end( frames ), frame ) != std::end( frames );
	}

	inline bool isDumpFrame( uint64_t frame )
	{
		static uint64_t const frames[]{ 3, 10, 30, 100, 200, 450, 900, 1800, 3600 };
		return std::find( std::begin( frames ), std::end( frames ), frame ) != std::end( frames );
	}

	inline bool isSwap( State const & s, ID3D11Resource * resource )
	{
		return resource
			&& std::find( s.swapImages.begin(), s.swapImages.end(), resource ) != s.swapImages.end();
	}

	inline void registerSwapImage( ID3D11Resource * resource )
	{
		if ( !resource )
		{
			return;
		}
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		if ( !isSwap( s, resource ) )
		{
			s.swapImages.push_back( resource );
		}
	}

	inline void unregisterSwapImage( ID3D11Resource * resource )
	{
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		s.swapImages.erase( std::remove( s.swapImages.begin(), s.swapImages.end(), resource ), s.swapImages.end() );
	}

	inline bool texSize( ID3D11Resource * resource, UINT & width, UINT & height, UINT & format, UINT & mips )
	{
		width = height = format = mips = 0u;
		ID3D11Texture2D * texture{};
		if ( !resource
			|| FAILED( resource->QueryInterface( __uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &texture ) ) )
			|| !texture )
		{
			return false;
		}
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc( &desc );
		texture->Release();
		width = desc.Width;
		height = desc.Height;
		format = UINT( desc.Format );
		mips = desc.MipLevels;
		return true;
	}

	inline UINT rtvSubresource( ID3D11RenderTargetView * view, DXGI_FORMAT & viewFormat, ID3D11Resource * resource )
	{
		D3D11_RENDER_TARGET_VIEW_DESC desc{};
		view->GetDesc( &desc );
		viewFormat = desc.Format;
		UINT w, h, f, mips;
		texSize( resource, w, h, f, mips );
		mips = std::max( mips, 1u );
		switch ( desc.ViewDimension )
		{
		case D3D11_RTV_DIMENSION_TEXTURE2D:
			return desc.Texture2D.MipSlice;
		case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
			return D3D11CalcSubresource( desc.Texture2DArray.MipSlice, desc.Texture2DArray.FirstArraySlice, mips );
		default:
			return 0u;
		}
	}

	inline bool srvSubresource( ID3D11ShaderResourceView * view, UINT & subresource, DXGI_FORMAT & viewFormat, ID3D11Resource * resource )
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		view->GetDesc( &desc );
		viewFormat = desc.Format;
		UINT w, h, f, mips;
		texSize( resource, w, h, f, mips );
		mips = std::max( mips, 1u );
		switch ( desc.ViewDimension )
		{
		case D3D11_SRV_DIMENSION_TEXTURE2D:
			subresource = desc.Texture2D.MostDetailedMip;
			return true;
		case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
			subresource = D3D11CalcSubresource( desc.Texture2DArray.MostDetailedMip, desc.Texture2DArray.FirstArraySlice, mips );
			return true;
		default:
			return false;
		}
	}

	// ---------------------------------------------------------------- draw-time state

	inline std::string describeState( ID3D11DeviceContext * ctx, ID3D11RenderTargetView * const * rtvs, ID3D11DepthStencilView * dsv, int slot )
	{
		std::string out;
		UINT rtvCount = 0u;
		for ( UINT i = 0u; i < 8u; ++i )
		{
			rtvCount += rtvs[ i ] ? 1u : 0u;
		}
		{
			D3D11_RENDER_TARGET_VIEW_DESC rd{};
			ID3D11Resource * res{};
			UINT w = 0u, h = 0u, f = 0u, mips = 0u;
			if ( slot >= 0 && rtvs[ slot ] )
			{
				rtvs[ slot ]->GetDesc( &rd );
				rtvs[ slot ]->GetResource( &res );
				texSize( res, w, h, f, mips );
				release( res );
			}
			out += fmt( " rt{slot=%d,viewFmt=%u,resFmt=%u,res=%ux%u,dim=%u,rtvs=%u}", slot, UINT( rd.Format ), f, w, h, UINT( rd.ViewDimension ), rtvCount );
		}
		if ( dsv )
		{
			D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
			dsv->GetDesc( &dd );
			out += fmt( " dsv{fmt=%u,flags=%u}", UINT( dd.Format ), UINT( dd.Flags ) );
		}
		else
		{
			out += " dsv{none}";
		}
		{
			D3D11_VIEWPORT vps[ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ]{};
			UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ctx->RSGetViewports( &count, vps );
			if ( count )
			{
				out += fmt( " vp{n=%u,%.1f,%.1f,%.1fx%.1f,z=%.2f..%.2f}", count, vps[ 0 ].TopLeftX, vps[ 0 ].TopLeftY, vps[ 0 ].Width, vps[ 0 ].Height, vps[ 0 ].MinDepth, vps[ 0 ].MaxDepth );
			}
			else
			{
				out += " vp{n=0}";
			}
			D3D11_RECT rects[ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ]{};
			count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ctx->RSGetScissorRects( &count, rects );
			if ( count )
			{
				out += fmt( " sc{n=%u,%ld,%ld,%ld,%ld}", count, rects[ 0 ].left, rects[ 0 ].top, rects[ 0 ].right, rects[ 0 ].bottom );
			}
			else
			{
				out += " sc{n=0}";
			}
		}
		{
			ID3D11RasterizerState * rs{};
			ctx->RSGetState( &rs );
			if ( rs )
			{
				D3D11_RASTERIZER_DESC d{};
				rs->GetDesc( &d );
				out += fmt( " rs{fill=%u,cull=%u,ccw=%d,scissor=%d,clip=%d,bias=%d}", UINT( d.FillMode ), UINT( d.CullMode ), int( d.FrontCounterClockwise ), int( d.ScissorEnable ), int( d.DepthClipEnable ), d.DepthBias );
				release( rs );
			}
			else
			{
				out += " rs{default}";
			}
		}
		{
			ID3D11BlendState * bs{};
			FLOAT factor[ 4 ]{};
			UINT mask = 0u;
			ctx->OMGetBlendState( &bs, factor, &mask );
			if ( bs )
			{
				D3D11_BLEND_DESC d{};
				bs->GetDesc( &d );
				auto const & t = d.RenderTarget[ ( d.IndependentBlendEnable && slot > 0 ) ? slot : 0 ];
				out += fmt( " bs{a2c=%d,ind=%d,blend=%d,src=%u,dst=%u,op=%u,srcA=%u,dstA=%u,opA=%u,mask=0x%X}", int( d.AlphaToCoverageEnable ), int( d.IndependentBlendEnable ), int( t.BlendEnable ), UINT( t.SrcBlend ), UINT( t.DestBlend ), UINT( t.BlendOp ), UINT( t.SrcBlendAlpha ), UINT( t.DestBlendAlpha ), UINT( t.BlendOpAlpha ), UINT( t.RenderTargetWriteMask ) );
				release( bs );
			}
			else
			{
				out += " bs{default}";
			}
			out += fmt( " sampleMask=0x%X", mask );
		}
		{
			ID3D11DepthStencilState * ds{};
			UINT ref = 0u;
			ctx->OMGetDepthStencilState( &ds, &ref );
			if ( ds )
			{
				D3D11_DEPTH_STENCIL_DESC d{};
				ds->GetDesc( &d );
				out += fmt( " ds{depth=%d,write=%u,func=%u,stencil=%d,ref=%u}", int( d.DepthEnable ), UINT( d.DepthWriteMask ), UINT( d.DepthFunc ), int( d.StencilEnable ), ref );
				release( ds );
			}
			else
			{
				out += " ds{default}";
			}
		}
		{
			ID3D11VertexShader * vs{};
			ID3D11PixelShader * ps{};
			ID3D11GeometryShader * gs{};
			ID3D11InputLayout * il{};
			ctx->VSGetShader( &vs, nullptr, nullptr );
			ctx->PSGetShader( &ps, nullptr, nullptr );
			ctx->GSGetShader( &gs, nullptr, nullptr );
			ctx->IAGetInputLayout( &il );
			out += fmt( " sh{vs=%d,ps=%d,gs=%d,il=%d}", vs ? 1 : 0, ps ? 1 : 0, gs ? 1 : 0, il ? 1 : 0 );
			release( vs );
			release( ps );
			release( gs );
			release( il );
		}
		{
			ID3D11ShaderResourceView * srvs[ 4 ]{};
			ctx->PSGetShaderResources( 0u, 4u, srvs );
			out += " srv{";
			for ( UINT i = 0u; i < 4u; ++i )
			{
				if ( !srvs[ i ] )
				{
					out += fmt( "%s%u:-", i ? "," : "", i );
					continue;
				}
				D3D11_SHADER_RESOURCE_VIEW_DESC d{};
				srvs[ i ]->GetDesc( &d );
				ID3D11Resource * res{};
				srvs[ i ]->GetResource( &res );
				UINT w = 0u, h = 0u, f = 0u, mips = 0u;
				texSize( res, w, h, f, mips );
				out += fmt( "%s%u:fmt=%u/%u:%ux%u:mips=%u:dim=%u%s", i ? "," : "", i, UINT( d.Format ), f, w, h, mips, UINT( d.ViewDimension ), isSwap( state(), res ) ? ":SWAP" : "" );
				release( res );
				release( srvs[ i ] );
			}
			out += "}";
			ID3D11Buffer * cbs[ 4 ]{};
			ctx->PSGetConstantBuffers( 0u, 4u, cbs );
			UINT cbCount = 0u;
			for ( auto & cb : cbs )
			{
				cbCount += cb ? 1u : 0u;
				release( cb );
			}
			ID3D11SamplerState * samplers[ 2 ]{};
			ctx->PSGetSamplers( 0u, 2u, samplers );
			out += fmt( " psCb=%u samplers=%d%d", cbCount, samplers[ 0 ] ? 1 : 0, samplers[ 1 ] ? 1 : 0 );
			release( samplers[ 0 ] );
			release( samplers[ 1 ] );
		}
		return out;
	}

	// Copies [offset, offset+size) of a GPU buffer to a staging buffer and reads it (diagnostic only).
	inline std::vector< uint8_t > readBuffer( ID3D11DeviceContext * ctx, ID3D11Buffer * buffer, UINT offset, UINT size )
	{
		std::vector< uint8_t > result;
		if ( !ctx || !buffer || !size )
		{
			return result;
		}
		D3D11_BUFFER_DESC desc{};
		buffer->GetDesc( &desc );
		if ( offset >= desc.ByteWidth )
		{
			return result;
		}
		size = std::min( size, desc.ByteWidth - offset );
		ID3D11Device * device{};
		ctx->GetDevice( &device );
		if ( !device )
		{
			return result;
		}
		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = ( size + 15u ) & ~15u;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Buffer * staging{};
		if ( SUCCEEDED( device->CreateBuffer( &stagingDesc, nullptr, &staging ) ) && staging )
		{
			D3D11_BOX box{ offset, 0u, 0u, offset + size, 1u, 1u };
			ctx->CopySubresourceRegion( staging, 0u, 0u, 0u, 0u, buffer, 0u, &box );
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if ( SUCCEEDED( ctx->Map( staging, 0u, D3D11_MAP_READ, 0u, &mapped ) ) && mapped.pData )
			{
				auto const * bytes = static_cast< uint8_t const * >( mapped.pData );
				result.assign( bytes, bytes + size );
				ctx->Unmap( staging, 0u );
			}
			staging->Release();
		}
		device->Release();
		return result;
	}

	inline std::string floats( std::vector< uint8_t > const & bytes, size_t maxFloats )
	{
		std::string out;
		size_t const count = std::min( maxFloats, bytes.size() / 4u );
		for ( size_t i = 0u; i < count; ++i )
		{
			float f;
			std::memcpy( &f, bytes.data() + i * 4u, 4u );
			out += fmt( "%s%.4g", i ? ";" : "", double( f ) );   // v13.25: ';' (comma decimals on Dutch Windows made ',' ambiguous)
		}
		return out;
	}

	inline std::string hexBytes( std::vector< uint8_t > const & bytes, size_t maxBytes )
	{
		std::string out;
		size_t const count = std::min( maxBytes, bytes.size() );
		for ( size_t i = 0u; i < count; ++i )
		{
			out += fmt( "%02X", UINT( bytes[ i ] ) );
			if ( ( i & 3u ) == 3u && i + 1u < count )
			{
				out += ' ';
			}
		}
		return out;
	}

	inline bool allZero( std::vector< uint8_t > const & bytes )
	{
		return std::all_of( bytes.begin(), bytes.end(), []( uint8_t b ){ return b == 0u; } );
	}

	// Vertex/index/constant data the GPU actually sees for one draw.
	inline void describeData( ID3D11DeviceContext * ctx, uint64_t frame, uint32_t drawIndex, uint32_t firstIndex, int32_t baseVertex )
	{
		std::string line = fmt( "RW_HD5500_FRAME_DIAG draw-data frame=%llu i=%u firstIndex=%u baseVertex=%d", (unsigned long long)frame, drawIndex, firstIndex, baseVertex );
		ID3D11Buffer * ib{};
		DXGI_FORMAT ibFormat{};
		UINT ibOffset = 0u;
		ctx->IAGetIndexBuffer( &ib, &ibFormat, &ibOffset );
		uint32_t firstVertexIndex = 0u;
		if ( ib )
		{
			UINT const indexBytes = ibFormat == DXGI_FORMAT_R32_UINT ? 4u : 2u;
			auto const data = readBuffer( ctx, ib, ibOffset + firstIndex * indexBytes, 6u * indexBytes );
			line += fmt( " ib{fmt=%u,off=%u,idx=", UINT( ibFormat ), ibOffset );
			for ( size_t i = 0u; i + indexBytes <= data.size(); i += indexBytes )
			{
				uint32_t v = 0u;
				std::memcpy( &v, data.data() + i, indexBytes );
				if ( i == 0u )
				{
					firstVertexIndex = v;
				}
				line += fmt( "%s%u", i ? "," : "", v );
			}
			line += "}";
			release( ib );
		}
		else
		{
			line += " ib{none}";
		}
		ID3D11Buffer * vbs[ 2 ]{};
		UINT strides[ 2 ]{};
		UINT offsets[ 2 ]{};
		ctx->IAGetVertexBuffers( 0u, 2u, vbs, strides, offsets );
		for ( UINT slot = 0u; slot < 2u; ++slot )
		{
			if ( !vbs[ slot ] )
			{
				continue;
			}
			D3D11_BUFFER_DESC desc{};
			vbs[ slot ]->GetDesc( &desc );
			UINT const stride = std::max( strides[ slot ], 4u );
			auto const start = offsets[ slot ] + UINT( std::max( 0, int32_t( firstVertexIndex ) + baseVertex ) ) * stride;
			auto const data = readBuffer( ctx, vbs[ slot ], start, std::min( 3u * stride, 96u ) );
			line += fmt( " vb%u{bytes=%u,usage=%u,stride=%u,off=%u,zero=%d,f=[%s],x=[%s]}", slot, desc.ByteWidth, UINT( desc.Usage ), strides[ slot ], offsets[ slot ], allZero( data ) ? 1 : 0, floats( data, 12u ).c_str(), hexBytes( data, 32u ).c_str() );
			release( vbs[ slot ] );
		}
		auto constants = [&]( char const * stage, ID3D11Buffer * const * cbs )
		{
			for ( UINT slot = 0u; slot < 4u; ++slot )
			{
				if ( !cbs[ slot ] )
				{
					continue;
				}
				D3D11_BUFFER_DESC desc{};
				cbs[ slot ]->GetDesc( &desc );
				auto const data = readBuffer( ctx, cbs[ slot ], 0u, std::min( desc.ByteWidth, 128u ) );
				line += fmt( " %scb%u{bytes=%u,usage=%u,zero=%d,f=[%s]}", stage, slot, desc.ByteWidth, UINT( desc.Usage ), allZero( data ) ? 1 : 0, floats( data, 32u ).c_str() );
			}
		};
		ID3D11Buffer * vscbs[ 4 ]{};
		ctx->VSGetConstantBuffers( 0u, 4u, vscbs );
		constants( "vs", vscbs );
		ID3D11Buffer * pscbs[ 4 ]{};
		ctx->PSGetConstantBuffers( 0u, 4u, pscbs );
		constants( "ps", pscbs );
		for ( auto & cb : vscbs ) release( cb );
		for ( auto & cb : pscbs ) release( cb );
		emit( line );
	}


	// v13.23: data of an atlas draw with the real constant-buffer windows (firstConstant)
	// and the push-constant slot b13; vertex data of the first vertex.
	// v13.36: first two pixel-shader textures as "WxH/fmt[rt]@ptr/mMost+LevelsofTotal".
	inline std::string describeTextures( ID3D11DeviceContext * ctx )
	{
		// v13.40: up to 6 slots; trailing empty slots are omitted.
		ID3D11ShaderResourceView * lsrv[ 6 ]{};
		ctx->PSGetShaderResources( 0u, 6u, lsrv );
		UINT used = 2u;
		for ( UINT i = 2u; i < 6u; ++i ) if ( lsrv[ i ] ) used = i + 1u;
		std::string tex;
		for ( UINT i = 0u; i < 6u; ++i )
		{
			if ( i >= used ) { if ( lsrv[ i ] ) lsrv[ i ]->Release(); continue; }
			char part[96] = "-";
			if ( lsrv[ i ] )
			{
				ID3D11Resource * r{}; lsrv[ i ]->GetResource( &r );
				ID3D11Texture2D * t{};
				if ( r && SUCCEEDED( r->QueryInterface( __uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &t ) ) ) )
				{
					D3D11_TEXTURE2D_DESC d{}; t->GetDesc( &d );
					D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; lsrv[ i ]->GetDesc( &vd );
					UINT const vMost = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? vd.Texture2D.MostDetailedMip : 99u;
					UINT const vLevels = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? vd.Texture2D.MipLevels : 99u;
					std::snprintf( part, sizeof( part ), "%ux%u/%d%s@%p/m%u+%dof%u", d.Width, d.Height, int( d.Format ), ( d.BindFlags & D3D11_BIND_RENDER_TARGET ) ? "rt" : "", static_cast< void * >( r ), vMost, int( vLevels ), d.MipLevels );
				}
				else if ( r ) std::snprintf( part, sizeof( part ), "buf" );
				release( t ); release( r ); release( lsrv[ i ] );
			}
			tex += ( i ? "," : "" );
			tex += part;
		}
		return tex;
	}

	// v13.32: true when the vertex push-constant matrix (b13) carries a translation, i.e. the
	// draw places a local mesh (pawn bodies/heads, animals) instead of baked world vertices.
	inline bool hasModelTranslation( ID3D11DeviceContext * ctx )
	{
		ID3D11DeviceContext1 * ctx1{};
		ctx->QueryInterface( __uuidof( ID3D11DeviceContext1 ), reinterpret_cast< void ** >( &ctx1 ) );
		ID3D11Buffer * cb{}; UINT first = 0u, num = 0u;
		if ( ctx1 ) ctx1->VSGetConstantBuffers1( 13u, 1u, &cb, &first, &num );
		else ctx->VSGetConstantBuffers( 13u, 1u, &cb );
		if ( ctx1 ) ctx1->Release();
		bool result = false;
		if ( cb )
		{
			auto const data = readBuffer( ctx, cb, first * 16u, 64u );
			if ( data.size() >= 64u )
			{
				float m[ 16 ]; std::memcpy( m, data.data(), 64u );
				result = std::fabs( m[ 3 ] ) + std::fabs( m[ 7 ] ) + std::fabs( m[ 11 ] ) + std::fabs( m[ 12 ] ) + std::fabs( m[ 13 ] ) + std::fabs( m[ 14 ] ) > 0.001f;
			}
			cb->Release();
		}
		return result;
	}


	// v13.46: for instanced draws, what instance data the vertex shader can actually see at draw
	// time: output-merger UAVs (writable storage buffers, e.g. Waylith's _InstanceTransforms) and
	// VS buffer SRVs (read-only storage buffers), with their view range and the first 16 floats
	// the GPU holds there. "uav[1]=null" while the shader expects u1 = instance data lost.
	inline std::string describeBufferView( ID3D11DeviceContext * ctx, ID3D11View * view, bool uav )
	{
		if ( !view ) return "null";
		ID3D11Resource * res{}; view->GetResource( &res );
		std::string out;
		UINT first = 0u, num = 0u, flags = 0u;
		if ( uav )
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC d{}; static_cast< ID3D11UnorderedAccessView * >( view )->GetDesc( &d );
			if ( d.ViewDimension == D3D11_UAV_DIMENSION_BUFFER ) { first = d.Buffer.FirstElement; num = d.Buffer.NumElements; flags = d.Buffer.Flags; }
			else { out = fmt( "%p/dim%u", static_cast< void * >( view ), unsigned( d.ViewDimension ) ); release( res ); return out; }
		}
		else
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC d{}; static_cast< ID3D11ShaderResourceView * >( view )->GetDesc( &d );
			if ( d.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX ) { first = d.BufferEx.FirstElement; num = d.BufferEx.NumElements; flags = d.BufferEx.Flags; }
			else if ( d.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ) { first = d.Buffer.FirstElement; num = d.Buffer.NumElements; }
			else { release( res ); return ""; }
		}
		D3D11_RESOURCE_DIMENSION dim{}; res->GetType( &dim );
		if ( dim == D3D11_RESOURCE_DIMENSION_BUFFER )
		{
			auto * buffer = static_cast< ID3D11Buffer * >( res );
			D3D11_BUFFER_DESC bd{}; buffer->GetDesc( &bd );
			auto const data = readBuffer( ctx, buffer, first * 4u, 64u );   // raw views count 4-byte elements
			out = fmt( "%p{buf=%p,bytes=%u,first=%u,num=%u,flags=%u,f=[%s]}", static_cast< void * >( view ), static_cast< void * >( buffer ), bd.ByteWidth, first, num, flags, floats( data, 16u ).c_str() );
		}
		release( res );
		return out;
	}

	inline void describeInstanceData( ID3D11DeviceContext * ctx, uint64_t frame, uint32_t draw, uint32_t instances, uint32_t count )
	{
		ID3D11RenderTargetView * rtvs[ 8 ]{}; ID3D11UnorderedAccessView * uavs[ 8 ]{};
		ctx->OMGetRenderTargetsAndUnorderedAccessViews( 8u, rtvs, nullptr, 0u, 8u, uavs );
		UINT rtvCount = 0u;
		for ( UINT i = 0u; i < 8u; ++i ) { if ( rtvs[ i ] ) { rtvCount = i + 1u; rtvs[ i ]->Release(); } }
		std::string line = fmt( "RW_HD5500_FRAME_DIAG inst-data frame=%llu i=%u count=%u inst=%u rtvs=%u", (unsigned long long)frame, draw, count, instances, rtvCount );
		for ( UINT i = rtvCount; i < 8u; ++i )
		{
			if ( uavs[ i ] || i < rtvCount + 3u ) line += fmt( " uav[%u]=%s", i, describeBufferView( ctx, uavs[ i ], true ).c_str() );
		}
		for ( auto & u : uavs ) if ( u ) u->Release();
		ID3D11ShaderResourceView * srvs[ 8 ]{};
		ctx->VSGetShaderResources( 0u, 8u, srvs );
		for ( UINT i = 0u; i < 8u; ++i )
		{
			if ( !srvs[ i ] ) continue;
			auto text = describeBufferView( ctx, srvs[ i ], false );
			if ( !text.empty() ) line += fmt( " vsSrv[%u]=%s", i, text.c_str() );
			srvs[ i ]->Release();
		}
		emit( line );
	}

	inline void describeAtlasDraw( ID3D11DeviceContext * ctx, uint64_t frame, uint32_t drawIndex, uint32_t firstIndex, int32_t baseVertex, bool indexed = true )
	{
		std::string line = fmt( "RW_HD5500_FRAME_DIAG atlas-draw frame=%llu i=%u kind=%s first=%u base=%d", (unsigned long long)frame, drawIndex, indexed ? "indexed" : "draw", firstIndex, baseVertex );
		ID3D11DeviceContext1 * ctx1{};
		ctx->QueryInterface( __uuidof( ID3D11DeviceContext1 ), reinterpret_cast< void ** >( &ctx1 ) );
		auto windows = [&]( char const * stage, bool vertex )
		{
			UINT const slots[] = { 0u, 1u, 2u, 3u, 13u };
			for ( UINT slot : slots )
			{
				ID3D11Buffer * cb{}; UINT first = 0u, num = 0u;
				if ( ctx1 )
				{
					if ( vertex ) ctx1->VSGetConstantBuffers1( slot, 1u, &cb, &first, &num );
					else ctx1->PSGetConstantBuffers1( slot, 1u, &cb, &first, &num );
				}
				else if ( vertex ) ctx->VSGetConstantBuffers( slot, 1u, &cb );
				else ctx->PSGetConstantBuffers( slot, 1u, &cb );
				if ( !cb ) continue;
				D3D11_BUFFER_DESC desc{}; cb->GetDesc( &desc );
				UINT const start = first * 16u;
				auto const data = start < desc.ByteWidth ? readBuffer( ctx, cb, start, std::min( desc.ByteWidth - start, 80u ) ) : std::vector< uint8_t >{};
				line += fmt( " %scb%u{bytes=%u,first=%u,num=%u,f=[%s]}", stage, slot, desc.ByteWidth, first, num, floats( data, 20u ).c_str() );
				release( cb );
			}
		};
		windows( "vs", true );
		windows( "ps", false );
		release( ctx1 );
		ID3D11Buffer * ib{}; DXGI_FORMAT ibFormat{}; UINT ibOffset = 0u;
		ctx->IAGetIndexBuffer( &ib, &ibFormat, &ibOffset );
		uint32_t firstVertexIndex = indexed ? 0u : firstIndex;   // v13.30: non-indexed draws start at firstVertex
		if ( ib && !indexed ) { release( ib ); ib = nullptr; }
		if ( ib )
		{
			UINT const indexBytes = ibFormat == DXGI_FORMAT_R32_UINT ? 4u : 2u;
			auto const data = readBuffer( ctx, ib, ibOffset + firstIndex * indexBytes, indexBytes );
			if ( data.size() == indexBytes ) std::memcpy( &firstVertexIndex, data.data(), indexBytes );
			release( ib );
		}
		ID3D11Buffer * vbs[ 4 ]{}; UINT strides[ 4 ]{}; UINT offsets[ 4 ]{};
		ctx->IAGetVertexBuffers( 0u, 4u, vbs, strides, offsets );
		for ( UINT slot = 0u; slot < 4u; ++slot )
		{
			if ( !vbs[ slot ] ) continue;
			UINT const stride = std::max( strides[ slot ], 4u );
			auto const start = offsets[ slot ] + UINT( std::max( 0, int32_t( firstVertexIndex ) + baseVertex ) ) * stride;
			auto const data = readBuffer( ctx, vbs[ slot ], start, std::min( stride, 64u ) );
			D3D11_BUFFER_DESC vbd{}; vbs[ slot ]->GetDesc( &vbd );
			line += fmt( " vb%u{stride=%u,off=%u,bytes=%u,f=[%s],x=[%s]}", slot, strides[ slot ], offsets[ slot ], vbd.ByteWidth, floats( data, 16u ).c_str(), hexBytes( data, 16u ).c_str() );
			release( vbs[ slot ] );
		}
		ID3D11InputLayout * il{}; ctx->IAGetInputLayout( &il );
		line += fmt( " il=%p", static_cast< void * >( il ) );
		release( il );
		emit( line );
	}

	inline void onDraw( ID3D11DeviceContext * ctx, char const * kind, uint32_t count, uint32_t instances, D3D11_PRIMITIVE_TOPOLOGY topology, uint32_t firstIndex = 0u, int32_t baseVertex = 0 )
	{
		if ( !enabled() || !ctx )
		{
			return;
		}
		auto & s = state();
		// v13.73: map tests (diagnostics on) took this mutex for every draw even in frames that are
		// not traced; read the lock-free copy first. RW_HD5500_DIAG_HINTS=0 = always lock.
		if ( hintsOn() && !s.traceHint.load( std::memory_order_relaxed ) ) return;
		std::lock_guard< std::mutex > lock{ s.mutex };
		if ( !s.trace )
		{
			return;
		}
		++s.draws;
		ID3D11RenderTargetView * rtvs[ 8 ]{};
		ID3D11DepthStencilView * dsv{};
		ctx->OMGetRenderTargets( 8u, rtvs, &dsv );
		ID3D11Resource * resources[ 8 ]{};
		int swapSlot = -1;
		for ( int i = 0; i < 8; ++i )
		{
			if ( rtvs[ i ] )
			{
				rtvs[ i ]->GetResource( &resources[ i ] );
				if ( swapSlot < 0 && isSwap( s, resources[ i ] ) )
				{
					swapSlot = i;
				}
			}
		}

		if ( swapSlot >= 0 )
		{
			++s.swapDraws;
			// v13.36: UI/swapchain draws also get a compact texture line (expansion logos etc.).
			if ( s.dump && s.swapTexLines < 200u )
			{
				++s.swapTexLines;
				ID3D11VertexShader * uvs{}; ID3D11PixelShader * ups{};
				ctx->VSGetShader( &uvs, nullptr, nullptr );
				ctx->PSGetShader( &ups, nullptr, nullptr );
				emit( fmt( "RW_HD5500_FRAME_DIAG drawlist-ui frame=%llu i=%u first=%u base=%d count=%u vs=%p ps=%p tex=%s"
					, (unsigned long long)s.frame, s.draws, firstIndex, baseVertex, count, static_cast< void * >( uvs ), static_cast< void * >( ups ), describeTextures( ctx ).c_str() ) );
				release( uvs ); release( ups );
			}
			if ( s.swapLines < 24u )
			{
				++s.swapLines;
				emit( fmt( "RW_HD5500_FRAME_DIAG draw-swap frame=%llu i=%u kind=%s count=%u inst=%u topo=%u", (unsigned long long)s.frame, s.draws, kind, count, instances, UINT( topology ) )
					+ describeState( ctx, rtvs, dsv, swapSlot ) );
			}
			if ( s.dataLines < 4u )
			{
				++s.dataLines;
				describeData( ctx, s.frame, s.draws, firstIndex, baseVertex );
			}
			if ( s.dump )
			{
				ID3D11ShaderResourceView * srvs[ 2 ]{};
				ctx->PSGetShaderResources( 0u, 2u, srvs );
				for ( UINT i = 0u; i < 2u; ++i )
				{
					s.lastSwapSrv[ i ].reset();
					if ( srvs[ i ] )
					{
						ID3D11Resource * res{};
						srvs[ i ]->GetResource( &res );
						UINT sub = 0u;
						DXGI_FORMAT viewFormat{};
						if ( res && srvSubresource( srvs[ i ], sub, viewFormat, res ) )
						{
							s.lastSwapSrv[ i ].set( res, sub, viewFormat );
						}
						release( res );
						release( srvs[ i ] );
					}
				}
			}
		}
		else
		{
			++s.otherDraws;
			// v13.23: in dump frames list EVERY offscreen draw compactly (shader objects,
			// first two textures, blend/depth), so a missing object (pawns, trees) can be
			// matched with its shader dump and texture.
			if ( s.dump && s.drawListLines < 600u )
			{
				++s.drawListLines;
				ID3D11VertexShader * vs{}; ID3D11PixelShader * ps{};
				ctx->VSGetShader( &vs, nullptr, nullptr );
				ctx->PSGetShader( &ps, nullptr, nullptr );
				std::string const tex = describeTextures( ctx );
				ID3D11BlendState * bs{}; FLOAT bf[4]{}; UINT sm{};
				ctx->OMGetBlendState( &bs, bf, &sm );
				D3D11_BLEND_DESC bd{}; if ( bs ) bs->GetDesc( &bd );
				ID3D11DepthStencilState * ds{}; UINT sref{};
				ctx->OMGetDepthStencilState( &ds, &sref );
				D3D11_DEPTH_STENCIL_DESC dd{}; if ( ds ) ds->GetDesc( &dd );
				UINT nvp = 1u; D3D11_VIEWPORT vp{}; ctx->RSGetViewports( &nvp, &vp );
				ID3D11SamplerState * smp{}; ctx->PSGetSamplers( 0u, 1u, &smp );
				D3D11_SAMPLER_DESC sd{}; if ( smp ) smp->GetDesc( &sd );
				release( smp );
				emit( fmt( "RW_HD5500_FRAME_DIAG drawlist frame=%llu i=%u kind=%s first=%u base=%d count=%u inst=%u vs=%p ps=%p tex=%s blend=%d/%d/%d mask=%X depth=%d/%d/%d vp=%.0fx%.0f smp=%X/%.3g/%.3g/%.3g/a%u"
					, (unsigned long long)s.frame, s.draws, kind, firstIndex, baseVertex, count, instances, static_cast< void * >( vs ), static_cast< void * >( ps ), tex.c_str()
					, int( bd.RenderTarget[0].BlendEnable ), int( bd.RenderTarget[0].SrcBlend ), int( bd.RenderTarget[0].DestBlend ), unsigned( bd.RenderTarget[0].RenderTargetWriteMask )
					, int( dd.DepthEnable ), int( dd.DepthWriteMask ), int( dd.DepthFunc ), vp.Width, vp.Height
					, unsigned( sd.Filter ), double( sd.MinLOD ), double( sd.MaxLOD ), double( sd.MipLODBias ), sd.MaxAnisotropy ) );
				release( bs ); release( ds );
				// v13.23: atlas draws (texture 0 is a render target >= 1024 wide) also log
				// their constant buffers and first vertex, at most 16 per frame.
				// v13.25: once per distinct (shaders, atlas size) combination per frame, so the
				// pawn atlas is not crowded out by the many earlier static-atlas draws.
				bool const atlasLike = tex.find( "rt" ) != std::string::npos && tex.find( "rt" ) < tex.find( ',' )
					&& std::atoi( tex.c_str() ) >= 1024;
				// v13.41: draws into single-channel float targets (RimWorld's WaterDepth height field,
				// R16) also log their vertex data, so a wrong per-vertex depth weight is visible.
				bool heightField = false;
				if ( rtvs[ 0 ] && resources[ 0 ] )
				{
					DXGI_FORMAT hfFormat{};
					rtvSubresource( rtvs[ 0 ], hfFormat, resources[ 0 ] );
					heightField = hfFormat == DXGI_FORMAT_R16_FLOAT || hfFormat == DXGI_FORMAT_R16_UNORM || hfFormat == DXGI_FORMAT_R32_FLOAT;
				}
				if ( instances > 1u && s.instUavLines < 24u ) { ++s.instUavLines; describeInstanceData( ctx, s.frame, s.draws, instances, count ); }
				if ( s.atlasDrawLines < 32u && ( atlasLike || heightField || hasModelTranslation( ctx ) ) )
				{
					std::string const key = fmt( "%p/%p/%s", static_cast< void * >( vs ), static_cast< void * >( ps ), tex.substr( 0u, tex.find( ',' ) ).c_str() );
					if ( s.atlasKeys.find( key ) == std::string::npos )
					{
						s.atlasKeys += key + "|";
						++s.atlasDrawLines;
						bool const indexed = std::strcmp( kind, "draw" ) != 0;
						describeAtlasDraw( ctx, s.frame, s.draws, firstIndex, baseVertex, indexed );
						if ( s.compareLines < 8u ) { ++s.compareLines; s.compareRequested = true; s.compareHint.store( true, std::memory_order_relaxed ); }
					}
				}
				release( vs ); release( ps );
			}
			if ( s.otherLines < 8u )
			{
				++s.otherLines;
				emit( fmt( "RW_HD5500_FRAME_DIAG draw-offscreen frame=%llu i=%u kind=%s count=%u inst=%u topo=%u", (unsigned long long)s.frame, s.draws, kind, count, instances, UINT( topology ) )
					+ describeState( ctx, rtvs, dsv, rtvs[ 0 ] ? 0 : -1 ) );
			}
			if ( s.dump && rtvs[ 0 ] && resources[ 0 ] )
			{
				DXGI_FORMAT viewFormat{};
				auto const sub = rtvSubresource( rtvs[ 0 ], viewFormat, resources[ 0 ] );
				s.lastOther.set( resources[ 0 ], sub, viewFormat );
				bool known = false;
				for ( uint32_t t = 0u; t < s.otherTargetCount; ++t ) known = known || ( s.otherTargets[ t ].resource == resources[ 0 ] && s.otherTargets[ t ].subresource == sub );
				if ( !known && s.otherTargetCount < 8u ) s.otherTargets[ s.otherTargetCount++ ].set( resources[ 0 ], sub, viewFormat );
			}
		}

		if ( s.dump )
		{
			// v13.22: remember every render-target texture the pixel shader samples.
			ID3D11ShaderResourceView * psSrvs[ 6 ]{};
			ctx->PSGetShaderResources( 0u, 6u, psSrvs );
			for ( UINT i = 0u; i < 6u; ++i )
			{
				if ( !psSrvs[ i ] ) continue;
				ID3D11Resource * res{};
				psSrvs[ i ]->GetResource( &res );
				ID3D11Texture2D * tex{};
				if ( res && SUCCEEDED( res->QueryInterface( __uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &tex ) ) ) )
				{
					D3D11_TEXTURE2D_DESC td{};
					tex->GetDesc( &td );
					bool const isSwapTarget = isSwap( s, res );
					bool already = false;
					for ( uint32_t t = 0u; t < s.otherTargetCount; ++t ) already = already || s.otherTargets[ t ].resource == res;
					for ( uint32_t t = 0u; t < s.sampledTargetCount; ++t ) already = already || s.sampledTargets[ t ].resource == res;
					UINT sub = 0u;
					DXGI_FORMAT viewFormat{};
					// v13.30: big atlases (>= 2048) may use the last 6 slots; small targets stop at 14.
					uint32_t const cap = ( td.Width >= 2048u || td.Height >= 2048u ) ? 20u : 14u;
					if ( ( td.BindFlags & ( D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS ) ) && !isSwapTarget && !already && s.sampledTargetCount < cap
						&& srvSubresource( psSrvs[ i ], sub, viewFormat, res ) )
					{
						D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; psSrvs[ i ]->GetDesc( &vd );
						UINT const vMost = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? vd.Texture2D.MostDetailedMip : 0u;
						UINT const vLevels = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? vd.Texture2D.MipLevels : 0u;
						emit( fmt( "RW_HD5500_FRAME_DIAG sampled-rt%u frame=%llu draw=%u slot=%u res=%ux%u@%p fmt=%d viewFmt=%d target=%s mips=%u view=%u+%u"
							, s.sampledTargetCount, (unsigned long long)s.frame, s.draws, i, td.Width, td.Height, static_cast< void * >( res ), int( td.Format ), int( viewFormat )
							, swapSlot >= 0 ? "swap" : "offscreen", td.MipLevels, vMost, vLevels ) );
						s.sampledTargets[ s.sampledTargetCount++ ].set( res, sub, viewFormat );
						// v13.30: also dump mip 2 of mipmapped render targets (what a minified quad samples).
						// v13.34: also atlases with only 2 levels (the plant atlas): dump the last of mips 1..2.
						if ( td.MipLevels > 1u && s.sampledTargetCount < cap )
						{
							UINT const extraMip = std::min( 2u, td.MipLevels - 1u );
							emit( fmt( "RW_HD5500_FRAME_DIAG sampled-rt%u frame=%llu draw=%u slot=%u res=%ux%u@%p mip=%u (extra)"
								, s.sampledTargetCount, (unsigned long long)s.frame, s.draws, i, td.Width, td.Height, static_cast< void * >( res ), extraMip ) );
							s.sampledTargets[ s.sampledTargetCount++ ].set( res, sub + extraMip, viewFormat );
						}
					}
				}
				release( tex );
				release( res );
				release( psSrvs[ i ] );
			}
		}

		for ( int i = 0; i < 8; ++i )
		{
			release( resources[ i ] );
			release( rtvs[ i ] );
		}
		release( dsv );
	}

	// ---------------------------------------------------------------- passes, clears, copies

	struct PassAttachment
	{
		ID3D11View * view{};
		bool colour{};
		int loadOp{};
		float clear[ 4 ]{};
	};

	inline void onPass( PassAttachment const * attachments, size_t count, UINT width, UINT height )
	{
		if ( !enabled() )
		{
			return;
		}
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		if ( !s.trace )
		{
			return;
		}
		int swapIndex = -1;
		for ( size_t i = 0u; i < count; ++i )
		{
			ID3D11Resource * res{};
			if ( attachments[ i ].view )
			{
				attachments[ i ].view->GetResource( &res );
			}
			if ( swapIndex < 0 && attachments[ i ].colour && isSwap( s, res ) )
			{
				swapIndex = int( i );
			}
			release( res );
		}
		if ( swapIndex >= 0 )
		{
			++s.swapPasses;
			if ( attachments[ swapIndex ].loadOp == 1 ) // VK_ATTACHMENT_LOAD_OP_CLEAR
			{
				++s.swapClears;
			}
		}
		else
		{
			++s.otherPasses;
		}
		if ( s.passLines >= 16u )
		{
			return;
		}
		++s.passLines;
		std::string line = fmt( "RW_HD5500_FRAME_DIAG pass frame=%llu target=%s fb=%ux%u attachments=%u", (unsigned long long)s.frame, swapIndex >= 0 ? "SWAP" : "offscreen", width, height, UINT( count ) );
		for ( size_t i = 0u; i < count; ++i )
		{
			ID3D11Resource * res{};
			if ( attachments[ i ].view )
			{
				attachments[ i ].view->GetResource( &res );
			}
			UINT w = 0u, h = 0u, f = 0u, mips = 0u;
			texSize( res, w, h, f, mips );
			release( res );
			auto const & a = attachments[ i ];
			line += fmt( " [%u:%s,fmt=%u,%ux%u,load=%d", UINT( i ), a.colour ? "col" : "ds", f, w, h, a.loadOp );
			if ( a.loadOp == 1 )
			{
				line += fmt( ",clear=%.3f,%.3f,%.3f,%.3f", a.clear[ 0 ], a.clear[ 1 ], a.clear[ 2 ], a.clear[ 3 ] );
			}
			line += "]";
		}
		emit( line );
	}

	inline void onClearView( ID3D11View * view, FLOAT const * colour, char const * source )
	{
		if ( !enabled() || !view )
		{
			return;
		}
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		if ( !s.trace )
		{
			return;
		}
		ID3D11Resource * res{};
		view->GetResource( &res );
		if ( isSwap( s, res ) )
		{
			++s.swapClears;
			emit( fmt( "RW_HD5500_FRAME_DIAG clear-swap frame=%llu source=%s colour=%.3f,%.3f,%.3f,%.3f", (unsigned long long)s.frame, source, colour ? colour[ 0 ] : -1.f, colour ? colour[ 1 ] : -1.f, colour ? colour[ 2 ] : -1.f, colour ? colour[ 3 ] : -1.f ) );
		}
		release( res );
	}

	inline void onCopyTo( ID3D11Resource * dst, ID3D11Resource * src, char const * source )
	{
		if ( !enabled() || !dst )
		{
			return;
		}
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		if ( !s.trace || !isSwap( s, dst ) )
		{
			return;
		}
		++s.swapCopies;
		UINT w = 0u, h = 0u, f = 0u, mips = 0u;
		texSize( src, w, h, f, mips );
		emit( fmt( "RW_HD5500_FRAME_DIAG copy-swap frame=%llu source=%s src=%ux%u,fmt=%u,mips=%u", (unsigned long long)s.frame, source, w, h, f, mips ) );
	}

	// Record-time: vkCmdBlitImage whose destination is a swapchain image (any frame, bounded).
	inline void onBlitRecorded( ID3D11Resource * src, ID3D11Resource * dst, uint32_t regions, int filter )
	{
		if ( !enabled() || !dst )
		{
			return;
		}
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		if ( !isSwap( s, dst ) || s.blitRecordLines >= 24u )
		{
			return;
		}
		++s.blitRecordLines;
		UINT w = 0u, h = 0u, f = 0u, mips = 0u;
		texSize( src, w, h, f, mips );
		emit( fmt( "RW_HD5500_FRAME_DIAG blit-to-swap-recorded frame=%llu src=%ux%u,fmt=%u regions=%u filter=%d", (unsigned long long)s.frame, w, h, f, regions, filter ) );
	}

	// ---------------------------------------------------------------- pixel dumps

	inline float halfToFloat( uint16_t h )
	{
		uint32_t const sign = ( h >> 15 ) & 1u;
		int32_t exponent = ( h >> 10 ) & 0x1F;
		uint32_t mantissa = h & 0x3FF;
		float value;
		if ( exponent == 0 )
		{
			value = std::ldexp( float( mantissa ), -24 );
		}
		else if ( exponent == 31 )
		{
			value = mantissa ? 0.0f : 65504.0f;
		}
		else
		{
			value = std::ldexp( float( mantissa | 0x400 ), exponent - 25 );
		}
		return sign ? -value : value;
	}

	inline float smallFloat( uint32_t bits, uint32_t mantissaBits )
	{
		uint32_t const exponent = bits >> mantissaBits;
		uint32_t const mantissa = bits & ( ( 1u << mantissaBits ) - 1u );
		if ( exponent == 0u )
		{
			return std::ldexp( float( mantissa ), -14 - int( mantissaBits ) );
		}
		if ( exponent == 31u )
		{
			return 65504.0f;
		}
		return std::ldexp( float( mantissa | ( 1u << mantissaBits ) ), int( exponent ) - 15 - int( mantissaBits ) );
	}

	inline uint8_t toByte( float v )
	{
		if ( !( v > 0.0f ) )
		{
			return 0u;
		}
		if ( v >= 1.0f )
		{
			return 255u;
		}
		return uint8_t( v * 255.0f + 0.5f );
	}

	// Decodes one pixel to RGBA8. Returns false for unsupported formats.
	// v13.41: first channel of a float target as a float, for min/mean/max statistics.
	inline bool decodeFloat0( DXGI_FORMAT format, uint8_t const * p, float & out )
	{
		switch ( format )
		{
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16G16_TYPELESS:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		{
			uint16_t v; std::memcpy( &v, p, 2u ); out = halfToFloat( v ); return true;
		}
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			std::memcpy( &out, p, 4u ); return true;
		default:
			return false;
		}
	}

	inline bool decode( DXGI_FORMAT format, uint8_t const * p, uint8_t * rgba )
	{
		switch ( format )
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT:
			rgba[ 0 ] = p[ 0 ]; rgba[ 1 ] = p[ 1 ]; rgba[ 2 ] = p[ 2 ]; rgba[ 3 ] = p[ 3 ];
			return true;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			rgba[ 0 ] = p[ 2 ]; rgba[ 1 ] = p[ 1 ]; rgba[ 2 ] = p[ 0 ]; rgba[ 3 ] = p[ 3 ];
			return true;
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			rgba[ 0 ] = p[ 2 ]; rgba[ 1 ] = p[ 1 ]; rgba[ 2 ] = p[ 0 ]; rgba[ 3 ] = 255u;
			return true;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		{
			uint32_t v;
			std::memcpy( &v, p, 4u );
			rgba[ 0 ] = uint8_t( ( v & 0x3FFu ) >> 2 );
			rgba[ 1 ] = uint8_t( ( ( v >> 10 ) & 0x3FFu ) >> 2 );
			rgba[ 2 ] = uint8_t( ( ( v >> 20 ) & 0x3FFu ) >> 2 );
			rgba[ 3 ] = uint8_t( ( v >> 30 ) * 85u );
			return true;
		}
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		{
			uint16_t v[ 4 ];
			std::memcpy( v, p, 8u );
			for ( int i = 0; i < 4; ++i )
			{
				rgba[ i ] = toByte( halfToFloat( v[ i ] ) );
			}
			return true;
		}
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		{
			uint16_t v[ 4 ];
			std::memcpy( v, p, 8u );
			for ( int i = 0; i < 4; ++i )
			{
				rgba[ i ] = uint8_t( v[ i ] >> 8 );
			}
			return true;
		}
		case DXGI_FORMAT_R11G11B10_FLOAT:
		{
			uint32_t v;
			std::memcpy( &v, p, 4u );
			rgba[ 0 ] = toByte( smallFloat( v & 0x7FFu, 6u ) );
			rgba[ 1 ] = toByte( smallFloat( ( v >> 11 ) & 0x7FFu, 6u ) );
			rgba[ 2 ] = toByte( smallFloat( ( v >> 22 ) & 0x3FFu, 5u ) );
			rgba[ 3 ] = 255u;
			return true;
		}
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		{
			float v[ 4 ];
			std::memcpy( v, p, 16u );
			for ( int i = 0; i < 4; ++i )
			{
				rgba[ i ] = toByte( v[ i ] );
			}
			return true;
		}
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_A8_UNORM:
			rgba[ 0 ] = rgba[ 1 ] = rgba[ 2 ] = p[ 0 ]; rgba[ 3 ] = 255u;
			return true;
		case DXGI_FORMAT_R8G8_UNORM:
			rgba[ 0 ] = p[ 0 ]; rgba[ 1 ] = p[ 1 ]; rgba[ 2 ] = 0u; rgba[ 3 ] = 255u;
			return true;
		// v13.36: single/dual-channel 16-bit targets (the shadow/light passes), shown as grey.
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		{
			// v13.41: R = value as 0..1 (shadow/light passes), G = B = 0.5 + 2*value, so small
			// signed fields (the water height field, |h| < 0.25) show as grey ripples around 128.
			uint16_t v; std::memcpy( &v, p, 2u );
			float const f = halfToFloat( v );
			rgba[ 0 ] = toByte( f ); rgba[ 1 ] = rgba[ 2 ] = toByte( 0.5f + 2.0f * f ); rgba[ 3 ] = 255u;
			return true;
		}
		case DXGI_FORMAT_R16_UNORM:
		{
			uint16_t v; std::memcpy( &v, p, 2u );
			rgba[ 0 ] = rgba[ 1 ] = rgba[ 2 ] = uint8_t( v >> 8 ); rgba[ 3 ] = 255u;
			return true;
		}
		case DXGI_FORMAT_R16G16_TYPELESS:
		case DXGI_FORMAT_R16G16_FLOAT:
		{
			uint16_t v[ 2 ]; std::memcpy( v, p, 4u );
			rgba[ 0 ] = toByte( halfToFloat( v[ 0 ] ) ); rgba[ 1 ] = toByte( halfToFloat( v[ 1 ] ) ); rgba[ 2 ] = 0u; rgba[ 3 ] = 255u;
			return true;
		}
		case DXGI_FORMAT_R16G16_UNORM:
		{
			uint16_t v[ 2 ]; std::memcpy( v, p, 4u );
			rgba[ 0 ] = uint8_t( v[ 0 ] >> 8 ); rgba[ 1 ] = uint8_t( v[ 1 ] >> 8 ); rgba[ 2 ] = 0u; rgba[ 3 ] = 255u;
			return true;
		}
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT:
		{
			float v; std::memcpy( &v, p, 4u );
			rgba[ 0 ] = rgba[ 1 ] = rgba[ 2 ] = toByte( v ); rgba[ 3 ] = 255u;
			return true;
		}
		default:
			return false;
		}
	}

	inline UINT bytesPerPixel( DXGI_FORMAT format )
	{
		switch ( format )
		{
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_A8_UNORM:
			return 1u;
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16_UNORM:
			return 2u;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
			return 8u;
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			return 16u;
		default:
			return 4u;
		}
	}

	inline bool writeBmp( std::string const & path, std::vector< uint8_t > const & rgba, UINT width, UINT height )
	{
		UINT const step = std::max( 1u, ( width + 639u ) / 640u );
		UINT const ow = std::max( 1u, width / step );
		UINT const oh = std::max( 1u, height / step );
		UINT const rowBytes = ( ow * 3u + 3u ) & ~3u;
		UINT const imageBytes = rowBytes * oh;
		uint8_t header[ 54 ]{};
		auto put32 = [&header]( size_t offset, uint32_t value )
		{
			std::memcpy( header + offset, &value, 4u );
		};
		header[ 0 ] = 'B';
		header[ 1 ] = 'M';
		put32( 2, 54u + imageBytes );
		put32( 10, 54u );
		put32( 14, 40u );
		put32( 18, ow );
		put32( 22, oh );
		header[ 26 ] = 1u;
		header[ 28 ] = 24u;
		put32( 34, imageBytes );
		put32( 38, 2835u );
		put32( 42, 2835u );
		FILE * file = nullptr;
		if ( fopen_s( &file, path.c_str(), "wb" ) != 0 || !file )
		{
			return false;
		}
		std::fwrite( header, 1u, sizeof( header ), file );
		std::vector< uint8_t > row( rowBytes, 0u );
		for ( UINT y = 0u; y < oh; ++y )
		{
			UINT const sy = ( oh - 1u - y ) * step; // BMP rows are bottom-up
			for ( UINT x = 0u; x < ow; ++x )
			{
				auto const * px = rgba.data() + ( size_t( sy ) * width + size_t( x ) * step ) * 4u;
				row[ x * 3u + 0u ] = px[ 2 ];
				row[ x * 3u + 1u ] = px[ 1 ];
				row[ x * 3u + 2u ] = px[ 0 ];
			}
			std::fwrite( row.data(), 1u, rowBytes, file );
		}
		std::fclose( file );
		return true;
	}

	// Caller holds the state mutex.
	inline void dumpTarget( ID3D11Device * device, ID3D11DeviceContext * ctx, ID3D11Resource * resource, UINT subresource, DXGI_FORMAT viewFormat, uint64_t frame, char const * tag )
	{
		framestats::g_diagnosticWork.fetch_add( 1u, std::memory_order_relaxed );   // v13.55: frame excluded from gameplay stats
		if ( !resource )
		{
			emit( fmt( "RW_HD5500_FRAME_DIAG dump frame=%llu tag=%s none", (unsigned long long)frame, tag ) );
			return;
		}
		ID3D11Texture2D * texture{};
		if ( FAILED( resource->QueryInterface( __uuidof( ID3D11Texture2D ), reinterpret_cast< void ** >( &texture ) ) ) || !texture )
		{
			emit( fmt( "RW_HD5500_FRAME_DIAG dump frame=%llu tag=%s not-texture2d", (unsigned long long)frame, tag ) );
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc( &desc );
		UINT const mips = std::max( desc.MipLevels, 1u );
		UINT const mip = subresource % mips;
		UINT const width = std::max( 1u, desc.Width >> mip );
		UINT const height = std::max( 1u, desc.Height >> mip );
		DXGI_FORMAT const format = viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : desc.Format;
		std::string head = fmt( "RW_HD5500_FRAME_DIAG dump frame=%llu tag=%s res=%ux%u mip=%u sub=%u resFmt=%u viewFmt=%u samples=%u", (unsigned long long)frame, tag, width, height, mip, subresource, UINT( desc.Format ), UINT( viewFormat ), desc.SampleDesc.Count );
		uint8_t const zero[ 16 ]{};
		uint8_t probe[ 4 ];
		if ( desc.SampleDesc.Count != 1u || !decode( format, zero, probe ) )
		{
			emit( head + " decoded=0 (unsupported format or MSAA)" );
			texture->Release();
			return;
		}
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Width = width;
		stagingDesc.Height = height;
		stagingDesc.MipLevels = 1u;
		stagingDesc.ArraySize = 1u;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0u;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags = 0u;
		ID3D11Texture2D * staging{};
		HRESULT hr = device->CreateTexture2D( &stagingDesc, nullptr, &staging );
		if ( FAILED( hr ) || !staging )
		{
			emit( head + fmt( " decoded=0 stagingHr=0x%08lX", (unsigned long)hr ) );
			texture->Release();
			return;
		}
		ctx->CopySubresourceRegion( staging, 0u, 0u, 0u, 0u, texture, subresource, nullptr );
		D3D11_MAPPED_SUBRESOURCE mapped{};
		hr = ctx->Map( staging, 0u, D3D11_MAP_READ, 0u, &mapped );
		if ( FAILED( hr ) || !mapped.pData )
		{
			emit( head + fmt( " decoded=0 mapHr=0x%08lX", (unsigned long)hr ) );
			staging->Release();
			texture->Release();
			return;
		}
		UINT const bpp = bytesPerPixel( format );
		std::vector< uint8_t > rgba( size_t( width ) * height * 4u );
		uint64_t nonBlack = 0u;
		uint64_t sum[ 3 ]{};
		UINT minX = width, minY = height, maxX = 0u, maxY = 0u;
		uint8_t alphaMin = 255u, alphaMax = 0u;
		uint32_t firstPixel = 0u;
		bool uniform = true;
		float fmin = 0.0f, fmax = 0.0f; double fsum = 0.0; uint64_t fcount = 0u, fnonzero = 0u;   // v13.41 float statistics
		for ( UINT y = 0u; y < height; ++y )
		{
			auto const * row = static_cast< uint8_t const * >( mapped.pData ) + size_t( y ) * mapped.RowPitch;
			for ( UINT x = 0u; x < width; ++x )
			{
				auto * out = rgba.data() + ( size_t( y ) * width + x ) * 4u;
				decode( format, row + size_t( x ) * bpp, out );
				float fv;
				if ( decodeFloat0( format, row + size_t( x ) * bpp, fv ) )
				{
					if ( !fcount ) { fmin = fmax = fv; }
					fmin = std::min( fmin, fv ); fmax = std::max( fmax, fv ); fsum += fv; ++fcount;
					if ( fv > 0.001f || fv < -0.001f ) ++fnonzero;
				}
				uint32_t packed;
				std::memcpy( &packed, out, 4u );
				if ( x == 0u && y == 0u )
				{
					firstPixel = packed;
				}
				else if ( packed != firstPixel )
				{
					uniform = false;
				}
				alphaMin = std::min( alphaMin, out[ 3 ] );
				alphaMax = std::max( alphaMax, out[ 3 ] );
				sum[ 0 ] += out[ 0 ];
				sum[ 1 ] += out[ 1 ];
				sum[ 2 ] += out[ 2 ];
				if ( out[ 0 ] | out[ 1 ] | out[ 2 ] )
				{
					++nonBlack;
					minX = std::min( minX, x );
					minY = std::min( minY, y );
					maxX = std::max( maxX, x );
					maxY = std::max( maxY, y );
				}
			}
		}
		ctx->Unmap( staging, 0u );
		staging->Release();
		texture->Release();
		uint64_t const total = uint64_t( width ) * height;
		std::string line = head + fmt( " decoded=1 nonBlack=%llu/%llu (%.2f%%) uniform=%d first=%02X%02X%02X%02X alpha=%u..%u meanRGB=%.1f,%.1f,%.1f",
			(unsigned long long)nonBlack, (unsigned long long)total, total ? 100.0 * double( nonBlack ) / double( total ) : 0.0,
			uniform ? 1 : 0,
			UINT( firstPixel & 0xFF ), UINT( ( firstPixel >> 8 ) & 0xFF ), UINT( ( firstPixel >> 16 ) & 0xFF ), UINT( firstPixel >> 24 ),
			UINT( alphaMin ), UINT( alphaMax ),
			total ? double( sum[ 0 ] ) / double( total ) : 0.0, total ? double( sum[ 1 ] ) / double( total ) : 0.0, total ? double( sum[ 2 ] ) / double( total ) : 0.0 );
		if ( nonBlack )
		{
			line += fmt( " bbox=%u,%u..%u,%u", minX, minY, maxX, maxY );
		}
		if ( fcount )
		{
			// v13.41: real range of float targets (the height field lives in +-0.1, invisible as bytes)
			line += fmt( " floatR=%.4f/%.4f/%.4f nonzero=%.2f%%", double( fmin ), fsum / double( fcount ), double( fmax ), 100.0 * double( fnonzero ) / double( fcount ) );
		}
		auto const & dir = dumpDir();
		if ( !dir.empty() )
		{
			auto const path = dir + fmt( "\\frame%05llu-%s.bmp", (unsigned long long)frame, tag );
			line += writeBmp( path, rgba, width, height ) ? ( " file=" + path ) : std::string( " file=WRITE-FAILED" );
			// v13.27: BMP has no alpha; atlases are mostly defined by alpha, so write it as grey.
			if ( alphaMin != alphaMax && std::strncmp( tag, "sampled", 7 ) == 0 )   // v13.29: only atlases
			{
				std::vector< uint8_t > alpha( rgba.size() );
				for ( size_t i = 0u; i + 3u < rgba.size(); i += 4u )
				{
					alpha[ i ] = alpha[ i + 1u ] = alpha[ i + 2u ] = rgba[ i + 3u ];
					alpha[ i + 3u ] = 255u;
				}
				writeBmp( dir + fmt( "\\frame%05llu-%s-alpha.bmp", (unsigned long long)frame, tag ), alpha, width, height );
			}
		}
		emit( line );
	}

	inline void onPresent( ID3D11Device * device, ID3D11DeviceContext * ctx, ID3D11Resource * presented, uint32_t imageIndex, uint32_t imageCount )
	{
		if ( !enabled() || !device || !ctx )
		{
			return;
		}
		auto & s = state();
		std::lock_guard< std::mutex > lock{ s.mutex };
		if ( s.trace )
		{
			emit( fmt( "RW_HD5500_FRAME_DIAG frame-summary frame=%llu draws=%u swapDraws=%u offscreenDraws=%u swapPasses=%u offscreenPasses=%u swapClears=%u swapCopies=%u image=%u/%u",
				(unsigned long long)s.frame, s.draws, s.swapDraws, s.otherDraws, s.swapPasses, s.otherPasses, s.swapClears, s.swapCopies, imageIndex, imageCount ) );
		}
		bool const dumped = s.dump;
		if ( s.dump )
		{
			dumpTarget( device, ctx, presented, 0u, DXGI_FORMAT_UNKNOWN, s.frame, "swapchain" );
		}
		// v13.70: the timed dumps (every 20 s before) wrote ~45 render targets per dump (up to
		// 4096x4096, ~200 MB of BMP) - since the project moved to the USB drive each one froze the
		// game for 2.5-8 s, and a dump slower than the interval triggered the next one at once
		// (44 s freeze in run 144726). Timed dumps now save only the swapchain picture, every 60 s
		// (RW_HD5500_FRAME_DUMP_SECONDS), and the timer restarts after the dump has finished.
		// The fixed early dump frames (3, 10, 30, ...) still save everything.
		// RW_HD5500_FRAME_DUMP_FULL=1 = full timed dumps as before.
		if ( s.dump && !s.dumpLight )
		{
			dumpTarget( device, ctx, s.lastSwapSrv[ 0 ].resource, s.lastSwapSrv[ 0 ].subresource, s.lastSwapSrv[ 0 ].viewFormat, s.frame, "swapdraw-srv0" );
			if ( s.lastSwapSrv[ 1 ].resource )
			{
				dumpTarget( device, ctx, s.lastSwapSrv[ 1 ].resource, s.lastSwapSrv[ 1 ].subresource, s.lastSwapSrv[ 1 ].viewFormat, s.frame, "swapdraw-srv1" );
			}
			// v13.29 (disk space): the last offscreen target is always also one of offscreen-rtN.
			{
				bool dup = false;
				for ( uint32_t t = 0u; t < s.otherTargetCount; ++t ) dup = dup || ( s.otherTargets[ t ].resource == s.lastOther.resource && s.otherTargets[ t ].subresource == s.lastOther.subresource );
				if ( !dup ) dumpTarget( device, ctx, s.lastOther.resource, s.lastOther.subresource, s.lastOther.viewFormat, s.frame, "last-offscreen-rt" );
			}
			for ( uint32_t t = 0u; t < s.otherTargetCount; ++t )
			{
				char tag[ 32 ];
				std::snprintf( tag, sizeof( tag ), "offscreen-rt%u", t );
				dumpTarget( device, ctx, s.otherTargets[ t ].resource, s.otherTargets[ t ].subresource, s.otherTargets[ t ].viewFormat, s.frame, tag );
			}
			for ( uint32_t t = 0u; t < s.sampledTargetCount; ++t )
			{
				char tag[ 32 ];
				std::snprintf( tag, sizeof( tag ), "sampled-rt%u", t );
				dumpTarget( device, ctx, s.sampledTargets[ t ].resource, s.sampledTargets[ t ].subresource, s.sampledTargets[ t ].viewFormat, s.frame, tag );
			}
		}
		{
			for ( uint32_t t = 0u; t < 20u; ++t ) s.sampledTargets[ t ].reset();
			s.sampledTargetCount = 0u;
		}
		for ( uint32_t t = 0u; t < 8u; ++t ) s.otherTargets[ t ].reset();
		s.otherTargetCount = 0u;
		s.lastSwapSrv[ 0 ].reset();
		s.lastSwapSrv[ 1 ].reset();
		s.lastOther.reset();
		s.draws = s.swapDraws = s.otherDraws = s.swapPasses = s.otherPasses = s.swapClears = s.swapCopies = 0u;
		s.swapLines = s.otherLines = s.passLines = s.dataLines = s.drawListLines = s.atlasDrawLines = s.instUavLines = s.compareLines = s.swapTexLines = 0u;
		s.compareRequested = false;
		s.atlasKeys.clear();
		++s.frame;
		s.trace = isTraceFrame( s.frame );
		s.dump = s.trace && isDumpFrame( s.frame );
		s.dumpLight = false;
		{
			static uint64_t const intervalMs = []()
				{
					char text[16]{};
					auto length = ::GetEnvironmentVariableA( "RW_HD5500_FRAME_DUMP_SECONDS", text, DWORD( sizeof( text ) ) );
					unsigned long v = ( length > 0u && length < sizeof( text ) ) ? std::strtoul( text, nullptr, 10 ) : 60ul;
					return uint64_t( v < 5ul ? 5ul : v ) * 1000u;
				}();
			static bool const fullTimed = readFlag( "RW_HD5500_FRAME_DUMP_FULL" );
			auto const nowMs = uint64_t( ::GetTickCount64() );
			if ( dumped || s.lastTimedTraceMs == 0u ) s.lastTimedTraceMs = nowMs;   // timer restarts after a dump
			if ( nowMs - s.lastTimedTraceMs >= intervalMs )
			{
				s.lastTimedTraceMs = nowMs;
				s.trace = true;
				if ( !s.dump ) s.dumpLight = !fullTimed;
				s.dump = true;
			}
		}
		s.traceHint.store( s.trace, std::memory_order_relaxed );   // v13.73
		s.compareHint.store( false, std::memory_order_relaxed );
	}
}
