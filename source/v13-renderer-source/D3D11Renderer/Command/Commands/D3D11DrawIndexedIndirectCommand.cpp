/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11DrawIndexedIndirectCommand.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"

#include "Buffer/D3D11Buffer.hpp"

#include "ashesd3d11_api.hpp"
#include "Command/Commands/D3D11IndexBind.hpp"

namespace ashes::D3D11_NAMESPACE
{
	DrawIndexedIndirectCommand::DrawIndexedIndirectCommand( VkDevice device
		, VkBuffer buffer
		, VkDeviceSize offset
		, uint32_t drawCount
		, uint32_t stride
		, VkPrimitiveTopology mode
		, VkIndexType type
		, SharedVbos vbos )
		: CommandBase{ device }
		, m_buffer{ buffer }
		, m_offset{ UINT( offset ) }
		, m_drawCount{ drawCount }
		, m_stride{ stride }
		, m_mode{ getPrimitiveTopology( mode ) }
		, m_type{ getIndexFormat( type ) }
		, m_vbos{ std::move( vbos ) }
	{
	}

	void DrawIndexedIndirectCommand::apply( Context & context )const
	{
		if ( context.stateCache.needsTopology( UINT( m_mode ) ) ) context.context->IASetPrimitiveTopology( m_mode );   // v13.52

		for ( auto const & binding : *m_vbos )
		{
			if ( context.stateCache.needsVertexBuffers( binding.startIndex, UINT( binding.d3dBuffers.size() ), binding.d3dBuffers.data(), binding.strides.data(), binding.offsets.data() ) )
				context.context->IASetVertexBuffers( binding.startIndex, UINT( binding.d3dBuffers.size() ), binding.d3dBuffers.data(), binding.strides.data(), binding.offsets.data() );   // v13.52
		}

		applyPendingIndex( context, false );   // v13.74: original index buffer (not the pool) for indirect draws
		// v13.66: indirect arguments come from a GPU buffer, so a folded index-buffer offset
		// cannot be added to them: bind the real offset first.
		if ( context.indexBase && context.indexBuffer )
		{
			UINT const size = context.indexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u;
			UINT const offset = context.indexBase * size;
			if ( context.stateCache.needsIndexBuffer( context.indexBuffer, UINT( context.indexFormat ), offset ) )
				context.context->IASetIndexBuffer( context.indexBuffer, context.indexFormat, offset );
			context.indexBase = 0u;
		}
		framediag::onDraw( *context.context, "indexed-indirect", 0u, 0u, m_mode );
		context.context->DrawIndexedInstancedIndirect( get( m_buffer )->getBuffer()
			, m_offset );
	}

	CommandPtr DrawIndexedIndirectCommand::clone()const
	{
		return std::make_unique< DrawIndexedIndirectCommand >( *this );
	}
}
