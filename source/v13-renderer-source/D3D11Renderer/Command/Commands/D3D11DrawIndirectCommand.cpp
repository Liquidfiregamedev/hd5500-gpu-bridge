/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11DrawIndirectCommand.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"

#include "Buffer/D3D11Buffer.hpp"

#include "ashesd3d11_api.hpp"

namespace ashes::D3D11_NAMESPACE
{
	DrawIndirectCommand::DrawIndirectCommand( VkDevice device
		, VkBuffer buffer
		, VkDeviceSize offset
		, uint32_t drawCount
		, uint32_t stride
		, VkPrimitiveTopology mode
		, SharedVbos vbos )
		: CommandBase{ device }
		, m_buffer{ buffer }
		, m_offset{ UINT( offset ) }
		, m_drawCount{ drawCount }
		, m_stride{ stride }
		, m_mode{ getPrimitiveTopology( mode ) }
		, m_vbos{ std::move( vbos ) }
	{
	}

	void DrawIndirectCommand::apply( Context & context )const
	{
		if ( context.stateCache.needsTopology( UINT( m_mode ) ) ) context.context->IASetPrimitiveTopology( m_mode );   // v13.52

		for ( auto const & binding : *m_vbos )
		{
			if ( context.stateCache.needsVertexBuffers( binding.startIndex, UINT( binding.d3dBuffers.size() ), binding.d3dBuffers.data(), binding.strides.data(), binding.offsets.data() ) )
				context.context->IASetVertexBuffers( binding.startIndex, UINT( binding.d3dBuffers.size() ), binding.d3dBuffers.data(), binding.strides.data(), binding.offsets.data() );   // v13.52
		}

		framediag::onDraw( *context.context, "indirect", 0u, 0u, m_mode );
		context.context->DrawInstancedIndirect( get( m_buffer )->getBuffer()
			, m_offset );
	}

	CommandPtr DrawIndirectCommand::clone()const
	{
		return std::make_unique< DrawIndirectCommand >( *this );
	}
}
