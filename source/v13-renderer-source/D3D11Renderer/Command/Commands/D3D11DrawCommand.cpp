/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11DrawCommand.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"
#include "Command/Commands/D3D11DrawDiag.hpp"

namespace ashes::D3D11_NAMESPACE
{
	DrawCommand::DrawCommand( VkDevice device
		, uint32_t vtxCount
		, uint32_t instCount
		, uint32_t firstVertex
		, uint32_t firstInstance
		, VkPrimitiveTopology mode
		, SharedVbos vbos )
		: CommandBase{ device }
		, m_vtxCount{ vtxCount }
		, m_instCount{ instCount }
		, m_firstVertex{ firstVertex }
		, m_firstInstance{ firstInstance }
		, m_mode{ getPrimitiveTopology( mode ) }
		, m_vbos{ std::move( vbos ) }
	{
	}

	void DrawCommand::apply( Context & context )const
	{
		// v13.52: topology and vertex buffers through the state cache (unchanged ones skipped).
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

		framediag::onDraw( *context.context, "draw", m_vtxCount, m_instCount, m_mode, m_firstVertex, 0 );
		if ( framediag::takeCompareRequest() )
		{
			drawdiag::compare( *context.context, *m_vbos, nullptr, 0u, 4u, m_firstVertex, 0, m_vtxCount, false );
		}

		if ( m_instCount > 1 )
		{
			context.context->DrawInstanced( m_vtxCount, m_instCount, m_firstVertex, m_firstInstance );
		}
		else
		{
			context.context->Draw( m_vtxCount, m_firstVertex );
		}
	}

	CommandPtr DrawCommand::clone()const
	{
		return std::make_unique< DrawCommand >( *this );
	}
}
