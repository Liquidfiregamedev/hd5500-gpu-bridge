/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

// v13.53: one buffer bound to several UAV slots at once.
//
// Vulkan lets a shader read several storage-buffer descriptors that point into the SAME
// VkBuffer at different offsets. Waylith does exactly that for instanced draws: one per-frame
// storage ring holds both `_InstanceTransforms` (u1) and `_Color` (u2) of a batch.
// D3D11 forbids it: "Two UAVs conflict if they share a subresource" (a buffer is one
// subresource), and with any conflict OMSetRenderTargetsAndUnorderedAccessViews is NOT passed
// to the driver at all — every UAV slot stays empty. The draw then reads zeros: all instance
// matrices are 0 and the whole batch disappears (RimWorld: steam geyser puffs, dust puffs and
// other flecks drawn with per-instance colours; the forbidden X has only u1 and was fine).
//
// Fix: the first slot that uses a buffer keeps the real view; every further slot on the same
// buffer gets a private scratch buffer into which its range is copied (GPU copy, in order on the
// immediate context) right before the bind. Graphics UAVs used this way are read-only instance
// data; a shader that WROTE through such an aliased slot would write into the scratch copy
// (logged once as a warning, see below).
// RW_GPU_UAV_ALIAS_COPY=0 switches the fix off (old behaviour, for comparison).

#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ashes::D3D11_NAMESPACE::uavalias
{
	inline bool enabled()
	{
		static bool const result = []()
			{
				auto value = std::getenv( "RW_GPU_UAV_ALIAS_COPY" );
				return !( value && value[0] == '0' );
			}();
		return result;
	}

	inline std::atomic< uint64_t > g_aliasCopies{ 0u };

	// Scratch buffer + view for one UAV slot, reused while big enough and of the same kind.
	struct Scratch
	{
		ID3D11Buffer * buffer{};
		ID3D11UnorderedAccessView * view{};
		UINT bytes{};
		UINT miscFlags{};
		UINT stride{};
		D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc{};

		void release()noexcept
		{
			if ( view ) view->Release();
			if ( buffer ) buffer->Release();
			view = nullptr;
			buffer = nullptr;
			bytes = 0u;
		}
	};

	inline void logFailure( char const * what, long hr = 0 )
	{
		static std::atomic< uint32_t > logged{ 0u };
		if ( logged++ < 6u ) std::fprintf( stderr, "[RW-GPU-UAV-ALIAS] private copy not possible: %s (hr=0x%08lX); original view kept\n", what, (unsigned long)hr );
	}

	// First bytes of a buffer range, read through a staging copy (diagnostic, first copies only).
	inline void readBack( ID3D11Device * device, ID3D11DeviceContext * ctx, ID3D11Buffer * buffer, UINT offset, float ( &out )[8] )
	{
		for ( auto & v : out ) v = -999.0f;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 32u; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Buffer * staging{};
		if ( FAILED( device->CreateBuffer( &bd, nullptr, &staging ) ) ) return;
		D3D11_BOX box{ offset, 0u, 0u, offset + 32u, 1u, 1u };
		ctx->CopySubresourceRegion( staging, 0u, 0u, 0u, 0u, buffer, 0u, &box );
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if ( SUCCEEDED( ctx->Map( staging, 0u, D3D11_MAP_READ, 0u, &mapped ) ) )
		{
			std::memcpy( out, mapped.pData, sizeof( out ) );
			ctx->Unmap( staging, 0u );
		}
		staging->Release();
	}

	inline Scratch & scratchFor( UINT slot )
	{
		static Scratch slots[64];   // D3D11.1 has at most 64 UAV slots
		return slots[slot < 64u ? slot : 63u];
	}

	// Bytes per element of a buffer UAV.
	inline UINT elementBytes( D3D11_UNORDERED_ACCESS_VIEW_DESC const & desc, D3D11_BUFFER_DESC const & bufferDesc )
	{
		if ( desc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW ) return 4u;
		if ( bufferDesc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED ) return bufferDesc.StructureByteStride;
		switch ( desc.Format )
		{
		case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16u;
		case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: return 8u;
		default: return 4u;
		}
	}

	// Returns a view on a private copy of `view`'s range, or nullptr when that is impossible
	// (not a buffer view, allocation failure): the caller then keeps the original view.
	inline ID3D11UnorderedAccessView * copyForSlot( ID3D11Device * device, ID3D11DeviceContext * ctx, UINT slot, ID3D11UnorderedAccessView * view )
	{
		D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
		view->GetDesc( &desc );
		if ( desc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER ) { logFailure( "not a buffer view" ); return nullptr; }
		ID3D11Resource * resource{};
		view->GetResource( &resource );
		auto * source = static_cast< ID3D11Buffer * >( resource );
		D3D11_BUFFER_DESC sourceDesc{};
		source->GetDesc( &sourceDesc );
		UINT const element = elementBytes( desc, sourceDesc );
		UINT const offset = desc.Buffer.FirstElement * element;
		UINT const bytes = desc.Buffer.NumElements * element;
		if ( !bytes || offset + bytes > sourceDesc.ByteWidth )
		{
			logFailure( "range outside the buffer" );
			resource->Release();
			return nullptr;
		}

		auto & scratch = scratchFor( slot );
		UINT const misc = sourceDesc.MiscFlags & ( D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_BUFFER_STRUCTURED );
		if ( !scratch.buffer || scratch.bytes < bytes || scratch.miscFlags != misc || scratch.stride != sourceDesc.StructureByteStride )
		{
			scratch.release();
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = ( ( bytes + 65535u ) / 65536u ) * 65536u;   // grow in 64 KiB steps
			if ( misc & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED ) bd.ByteWidth = ( bd.ByteWidth / sourceDesc.StructureByteStride ) * sourceDesc.StructureByteStride;
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			bd.MiscFlags = misc;
			bd.StructureByteStride = ( misc & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED ) ? sourceDesc.StructureByteStride : 0u;
			if ( HRESULT hr = device->CreateBuffer( &bd, nullptr, &scratch.buffer ); FAILED( hr ) )
			{
				logFailure( "CreateBuffer", hr );
				scratch.buffer = nullptr;
				resource->Release();
				return nullptr;
			}
			scratch.bytes = bd.ByteWidth;
			scratch.miscFlags = misc;
			scratch.stride = sourceDesc.StructureByteStride;
			scratch.viewDesc = {};
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC wanted = desc;
		wanted.Buffer.FirstElement = 0u;
		if ( !scratch.view || std::memcmp( &wanted, &scratch.viewDesc, sizeof( wanted ) ) != 0 )
		{
			if ( scratch.view ) scratch.view->Release();
			scratch.view = nullptr;
			if ( HRESULT hr = device->CreateUnorderedAccessView( scratch.buffer, &wanted, &scratch.view ); FAILED( hr ) )
			{
				logFailure( "CreateUnorderedAccessView", hr );
				scratch.view = nullptr;
				resource->Release();
				return nullptr;
			}
			scratch.viewDesc = wanted;
		}

		D3D11_BOX box{ offset, 0u, 0u, offset + bytes, 1u, 1u };
		ctx->CopySubresourceRegion( scratch.buffer, 0u, 0u, 0u, 0u, source, 0u, &box );
		static std::atomic< uint32_t > verified{ 0u };
		if ( verified++ < 2u )
		{
			float src[8], dst[8];
			readBack( device, ctx, source, offset, src );
			readBack( device, ctx, scratch.buffer, 0u, dst );
			std::fprintf( stderr, "[RW-GPU-UAV-ALIAS] verify slot=%u offset=%u bytes=%u srcMisc=0x%X srcBind=0x%X srcUsage=%u fmt=%d flags=0x%X src=[%g %g %g %g %g %g %g %g] copy=[%g %g %g %g %g %g %g %g]\n"
				, slot, offset, bytes, sourceDesc.MiscFlags, sourceDesc.BindFlags, unsigned( sourceDesc.Usage ), int( desc.Format ), desc.Buffer.Flags
				, src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7], dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7] );
		}
		resource->Release();
		g_aliasCopies.fetch_add( 1u, std::memory_order_relaxed );
		return scratch.view;
	}

	// Replace every view that shares its buffer with an earlier slot by a private copy.
	// `firstSlot` is the absolute UAV slot of views[0] (used to pick the scratch buffer).
	// Returns the number of slots replaced.
	inline UINT separate( ID3D11Device * device, ID3D11DeviceContext * ctx, UINT firstSlot, ID3D11UnorderedAccessView ** views, size_t count )
	{
		static bool const trace = std::getenv( "RW_GPU_UAV_ALIAS_TRACE" ) != nullptr;   // diagnostic only
		if ( trace )
		{
			std::fprintf( stderr, "[RW-GPU-UAV-ALIAS-TRACE] first=%u count=%zu", firstSlot, count );
			for ( size_t k = 0u; k < count; ++k ) { auto * v = views[k]; ID3D11Resource * r{}; if ( v ) { v->GetResource( &r ); r->Release(); } std::fprintf( stderr, " %p(res %p)", (void *)v, (void *)r ); }
			std::fprintf( stderr, "\n" );
		}
		if ( !enabled() || count < 2u ) return 0u;
		UINT replaced = 0u;
		// v13.63: fixed-size list instead of a heap vector (UAV ranges are small; the
		// output-merger limit is 64 slots).
		ID3D11Resource * seen[64]{};
		size_t seenCount = 0u;
		for ( size_t i = 0u; i < count; ++i )
		{
			if ( !views[i] ) continue;
			ID3D11Resource * resource{};
			views[i]->GetResource( &resource );
			bool duplicate = false;
			for ( size_t k = 0u; k < seenCount; ++k ) if ( seen[k] == resource ) duplicate = true;
			if ( !duplicate && seenCount < 64u ) seen[seenCount++] = resource;
			resource->Release();
			if ( !duplicate ) continue;
			if ( auto copy = copyForSlot( device, ctx, firstSlot + UINT( i ), views[i] ) )
			{
				views[i] = copy;
				++replaced;
			}
		}
		if ( replaced )
		{
			static std::atomic< uint32_t > logged{ 0u };
			if ( logged++ < 4u )
			{
				std::fprintf( stderr, "[RW-GPU-UAV-ALIAS] %u UAV slot(s) shared a buffer with an earlier slot; bound private copies (D3D11 would drop the whole UAV binding)\n", replaced );
			}
		}
		return replaced;
	}

	// The one place graphics (output-merger) UAVs are bound, keeping the render targets.
	// Applies separate() first. Returns true when private copies were bound (the caller must
	// then re-bind before every draw, the copied data can change per draw).
	inline bool bindOutput( ID3D11Device * device, ID3D11DeviceContext * ctx, UINT startSlot, UINT count, ID3D11UnorderedAccessView * const * views )
	{
		// v13.63: stack copy (was a heap vector per output-merger UAV bind).
		ID3D11UnorderedAccessView * bound[64]{};
		UINT const n = count < 64u ? count : 64u;
		for ( UINT k = 0u; k < n; ++k ) bound[k] = views[k];
		bool const aliased = separate( device, ctx, startSlot, bound, n ) != 0u;
		ctx->OMSetRenderTargetsAndUnorderedAccessViews( D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL
			, nullptr, nullptr, startSlot, n, n ? bound : nullptr, nullptr );
		return aliased;
	}
}
