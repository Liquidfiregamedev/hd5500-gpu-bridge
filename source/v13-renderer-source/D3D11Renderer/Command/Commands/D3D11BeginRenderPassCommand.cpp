/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11BeginRenderPassCommand.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"

#include "Buffer/D3D11BufferView.hpp"
#include "Core/D3D11Device.hpp"
#include "RenderPass/D3D11FrameBuffer.hpp"
#include "RenderPass/D3D11RenderPass.hpp"

#include "ashesd3d11_api.hpp"

namespace ashes::D3D11_NAMESPACE
{
	BeginRenderPassCommand::BeginRenderPassCommand( VkDevice device
		, VkRenderPass renderPass
		, VkFramebuffer frameBuffer
		, VkClearValueArray const & clearValues )
		: CommandBase{ device }
		, m_renderPass{ renderPass }
		, m_frameBuffer{ frameBuffer }
		, m_scissor{ makeScissor( get( m_frameBuffer )->getDimensions() ) }
	{
		auto rp = get( renderPass );
		assert( clearValues.size() == rp->size() );
		for ( auto const & attach : *rp )
		{
			auto & clearValue = clearValues[attach.attachment];

			if ( ashes::isDepthOrStencilFormat( rp->getAttachment( attach ).format ) )
			{
				m_dsClearValue = clearValue;
			}
			else
			{
				m_rtClearValues.push_back( clearValue );
			}
		}
	}

	void BeginRenderPassCommand::apply( Context & context )const
	{
		context.context->RSSetScissorRects( 1u, &m_scissor );
		auto & views = get( m_frameBuffer )->getAllViews();
		uint32_t clearIndex = 0u;

		if ( framediag::enabled() )
		{
			std::vector< framediag::PassAttachment > diag;
			uint32_t diagClear = 0u;
			for ( auto const & reference : *get( m_renderPass ) )
			{
				auto & attachDesc = get( m_renderPass )->getAttachment( reference );
				framediag::PassAttachment a{};
				a.view = reference.attachment < views.size() ? views[reference.attachment]->view : nullptr;
				a.colour = getAspectMask( attachDesc.format ) == VK_IMAGE_ASPECT_COLOR_BIT;
				a.loadOp = int( attachDesc.loadOp );
				if ( a.colour && attachDesc.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && diagClear < m_rtClearValues.size() )
				{
					for ( int c = 0; c < 4; ++c ) a.clear[c] = m_rtClearValues[diagClear].color.float32[c];
					++diagClear;
				}
				diag.push_back( a );
			}
			auto const dims = get( m_frameBuffer )->getDimensions();
			framediag::onPass( diag.data(), diag.size(), dims.width, dims.height );
		}

		for ( auto const & reference : *get( m_renderPass ) )
		{
			auto & attachDesc = get( m_renderPass )->getAttachment( reference );

			if ( attachDesc.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR )
			{
				if ( getAspectMask( attachDesc.format ) == VK_IMAGE_ASPECT_COLOR_BIT )
				{
					context.context->ClearRenderTargetView( static_cast< ID3D11RenderTargetView * >( views[reference.attachment]->view )
						, m_rtClearValues[clearIndex].color.float32 );
					++clearIndex;
				}
				else
				{
					context.context->ClearDepthStencilView( static_cast< ID3D11DepthStencilView * >( views[reference.attachment]->view )
						, get( m_frameBuffer )->getDSViewFlags()
						, m_dsClearValue.depthStencil.depth
						, UINT8( m_dsClearValue.depthStencil.stencil ) );
				}
			}
		}
	}

	CommandPtr BeginRenderPassCommand::clone()const
	{
		return std::make_unique< BeginRenderPassCommand >( *this );
	}
}
