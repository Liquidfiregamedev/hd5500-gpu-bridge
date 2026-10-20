/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11BindIndexBufferCommand.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"
#include "Miscellaneous/D3D11WatchedBytes.hpp"
#include "Miscellaneous/D3D11IndexPool.hpp"

#include "Buffer/D3D11Buffer.hpp"

#include "ashesd3d11_api.hpp"

namespace ashes::D3D11_NAMESPACE
{
	BindIndexBufferCommand::BindIndexBufferCommand( VkDevice device
		, VkBuffer ibo
		, uint64_t offset
		, VkIndexType indexType )
		: CommandBase{ device }
		, m_ibo{ ibo }
		, m_offset{ UINT( offset ) }
		, m_indexType{ getIndexFormat( indexType ) }
	{
	}

	void BindIndexBufferCommand::apply( Context & context )const
	{
		auto buffer = get( m_ibo )->getBuffer();
		// v13.74: index pool - remember the binding; the next indexed draw resolves it
		if ( idxpool::enabled() )
		{
			context.indexMemory = &get( m_ibo )->getObjectMemory();
			context.indexObjectBuffer = buffer;
			context.indexVkOffset = m_offset;
			context.indexFormat = m_indexType;
			context.indexPending = true;
			return;
		}
		context.indexMemory = nullptr;
		// v13.66: fold the offset into firstIndex (RW_GPU_INDEX_FOLD=0 switches it off).
		// D3D11 reads index i at offset + (StartIndexLocation + i) * size, so binding at offset 0
		// and drawing from StartIndexLocation + offset / size reads exactly the same indices.
		// Vulkan requires the offset to be a multiple of the index size; checked anyway.
		static bool const fold = envflags::on( "RW_GPU_INDEX_FOLD", true );
		UINT const size = m_indexType == DXGI_FORMAT_R16_UINT ? 2u : 4u;
		UINT offset = m_offset;
		UINT base = 0u;
		if ( fold && ( m_offset % size ) == 0u )
		{
			offset = 0u;
			base = m_offset / size;
		}
		if ( context.stateCache.needsIndexBuffer( buffer, UINT( m_indexType ), offset ) )   // v13.58
		{
			context.context->IASetIndexBuffer( buffer, m_indexType, offset );
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
		context.indexFormat = m_indexType;
	}

	uint64_t BindIndexBufferCommand::removeKey()const
	{
		return removeKeyStart( 3u );   // remove() always does IASetIndexBuffer( nullptr )
	}

	void BindIndexBufferCommand::remove( Context const & context )const
	{
		const_cast< Context & >( context ).stateCache.invalidateInputAssembly();   // v13.58: direct set
		const_cast< Context & >( context ).indexBase = 0u;   // v13.66
		const_cast< Context & >( context ).indexMemory = nullptr;   // v13.74
		const_cast< Context & >( context ).indexPending = false;
		context.context->IASetIndexBuffer( nullptr, DXGI_FORMAT_UNKNOWN, 0u );
	}

	CommandPtr BindIndexBufferCommand::clone()const
	{
		return std::make_unique< BindIndexBufferCommand >( *this );
	}
}
