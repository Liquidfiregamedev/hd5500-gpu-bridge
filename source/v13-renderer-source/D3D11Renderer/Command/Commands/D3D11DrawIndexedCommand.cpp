/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Miscellaneous/D3D11EnvFlags.hpp"
#include "Command/Commands/D3D11DrawIndexedCommand.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"
#include "Command/Commands/D3D11DrawDiag.hpp"
#include "Buffer/D3D11Buffer.hpp"
#include "ashesd3d11_api.hpp"
#include "Command/Commands/D3D11IndexBind.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>

namespace ashes::D3D11_NAMESPACE
{
	namespace drawidx
	{
		static uint32_t getSize( VkIndexType type )
		{
			switch ( type )
			{
			case VK_INDEX_TYPE_UINT16:
				return 2u;
			case VK_INDEX_TYPE_UINT32:
				return 4u;
			default:
				assert( false && "Unsupported index type" );
				return 1u;
			}
		}
	}

	DrawIndexedCommand::DrawIndexedCommand( VkDevice device
		, uint32_t indexCount
		, uint32_t instCount
		, uint32_t firstIndex
		, uint32_t vertexOffset
		, uint32_t firstInstance
		, VkPrimitiveTopology mode
		, VkIndexType type
		, SharedVbos vbos
		, VkBuffer diagIbo
		, VkDeviceSize diagIboOffset )
		: CommandBase{ device }
		, m_indexCount{ indexCount }
		, m_instCount{ instCount }
		, m_firstIndex{ firstIndex }
		, m_vertexOffset{ vertexOffset }
		, m_firstInstance{ firstInstance }
		, m_mode{ getPrimitiveTopology( mode ) }
		, m_type{ getIndexFormat( type ) }
		, m_size{ drawidx::getSize( type ) }
		, m_vbos{ std::move( vbos ) }
		, m_diagIbo{ diagIbo }
		, m_diagIboOffset{ diagIboOffset }
	{
	}

	void DrawIndexedCommand::apply( Context & context )const
	{
		if ( envflags::uiTrace() )   // v13.51: cached (was a getenv per draw)
		{
			static std::atomic< uint64_t > drawCount{ 0u };
			auto serial = ++drawCount;
			if ( serial <= 32u || ( serial & ( serial - 1u ) ) == 0u )
			{
				ID3D11Buffer * ibo{}; DXGI_FORMAT format{}; UINT offset{};
				context.context->IAGetIndexBuffer( &ibo, &format, &offset );
				std::cerr << "[RW-GPU-UI-DRAW] serial=" << serial << " indexed=1 indices=" << m_indexCount
					<< " instances=" << m_instCount << " firstIndex=" << m_firstIndex
					<< " vertexOffset=" << INT( m_vertexOffset ) << " ibo=" << ibo
					<< " iboOffset=" << offset << " iboFormat=" << int( format ) << std::endl;
				if ( ibo ) ibo->Release();
			}
		}
		// v13.52: topology and vertex buffers through the state cache (unchanged ones skipped).
		applyPendingIndex( context );   // v13.74: index pool
		if ( context.stateCache.needsTopology( UINT( m_mode ) ) ) context.context->IASetPrimitiveTopology( m_mode );

		for ( auto const & binding : *m_vbos )
		{
			if ( context.stateCache.needsVertexBuffers( binding.startIndex, UINT( binding.d3dBuffers.size() )
				, binding.d3dBuffers.data(), binding.strides.data(), binding.offsets.data() ) )
			{
				context.context->IASetVertexBuffers( binding.startIndex
					, UINT( binding.d3dBuffers.size() )
					, binding.d3dBuffers.data()
					, binding.strides.data()
					, binding.offsets.data() );
			}
		}

		UINT const firstIndex = m_firstIndex + context.indexBase;   // v13.66: folded index-buffer offset
		framediag::onDraw( *context.context, "indexed", m_indexCount, m_instCount, m_mode, firstIndex, INT( m_vertexOffset ) );
		if ( framediag::takeCompareRequest() )
		{
			drawdiag::compare( *context.context, *m_vbos, m_diagIbo, m_diagIboOffset, m_size, m_firstIndex, INT( m_vertexOffset ), m_indexCount, true );
		}

		if ( m_instCount != 1u || m_firstInstance != 0u )
		{
			context.context->DrawIndexedInstanced( m_indexCount
				, m_instCount
				, firstIndex
				, INT( m_vertexOffset )
				, m_firstInstance );
		}
		else
		{
			context.context->DrawIndexed( m_indexCount
				, firstIndex
				, INT( m_vertexOffset ) );
		}
	}

	CommandPtr DrawIndexedCommand::clone()const
	{
		return std::make_unique< DrawIndexedCommand >( *this );
	}
}
