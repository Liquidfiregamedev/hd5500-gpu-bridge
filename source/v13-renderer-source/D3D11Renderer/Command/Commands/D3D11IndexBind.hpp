/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

// v13.74: draw-time index-buffer binding for the index pool (see Miscellaneous/D3D11IndexPool.hpp).

#include "renderer/D3D11Renderer/Command/Commands/D3D11CommandBase.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11DeviceMemory.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11IndexPool.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11WatchedBytes.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11EnvFlags.hpp"

namespace ashes::D3D11_NAMESPACE
{
	// allowPool=false (indirect draws): bind the original buffer; the next draw re-resolves.
	inline void applyPendingIndex( Context & context, bool allowPool = true )
	{
		auto const * memory = context.indexMemory;
		if ( !memory ) return;   // pool off: BindIndexBufferCommand already bound it
		uint64_t const version = memory->contentVersion.load( std::memory_order_relaxed );
		if ( allowPool && !context.indexPending && version == context.indexResolvedVersion ) return;
		if ( !context.indexPoolChecked )
		{
			context.indexPoolChecked = true;
			context.indexPool = idxpool::pool( get( context.device )->getDevice() );
		}
		ID3D11Buffer * buffer = context.indexObjectBuffer;
		UINT offsetBytes = context.indexVkOffset;
		auto * pool = static_cast< idxpool::Pool * >( context.indexPool );
		UINT slot = 0u;
		if ( allowPool && pool && idxpool::enabled()
			&& pool->resolve( *context.context, memory->serial, version, memory->resource, slot ) )
		{
			buffer = pool->buffer();
			offsetBytes += slot;
		}
		static bool const fold = envflags::on( "RW_GPU_INDEX_FOLD", true );
		UINT const size = context.indexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u;
		UINT offset = offsetBytes;
		UINT base = 0u;
		if ( fold && ( offsetBytes % size ) == 0u )
		{
			offset = 0u;
			base = offsetBytes / size;
		}
		if ( context.stateCache.needsIndexBuffer( buffer, UINT( context.indexFormat ), offset ) )
		{
			context.context->IASetIndexBuffer( buffer, context.indexFormat, offset );
			g_indexBinds.fetch_add( 1u, std::memory_order_relaxed );
		}
		else if ( base != context.indexBase )
		{
			g_indexFolded.fetch_add( 1u, std::memory_order_relaxed );
		}
		else
		{
			g_indexSame.fetch_add( 1u, std::memory_order_relaxed );
		}
		context.indexBase = base;
		context.indexBuffer = buffer;
		context.indexPending = !allowPool;
		context.indexResolvedVersion = version;
	}
}
