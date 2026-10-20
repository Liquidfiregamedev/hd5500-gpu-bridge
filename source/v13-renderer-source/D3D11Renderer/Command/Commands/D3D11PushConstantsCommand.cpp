/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11PushConstantsCommand.hpp"

#include "Buffer/D3D11Buffer.hpp"
#include "Shader/D3D11ShaderModule.hpp"
#include "Miscellaneous/D3D11WatchedBytes.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"

#include "ashesd3d11_api.hpp"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	namespace pushconst
	{
		// v13.52: RW_GPU_PUSH_SKIP_SAME=0 uploads every push again (old behaviour).
		inline bool skipSame()
		{
			static bool const result = envflags::on( "RW_GPU_PUSH_SKIP_SAME", true );
			return result;
		}
	}

	PushConstantsCommand::PushConstantsCommand( VkDevice device
		, PushConstantsBuffer pcb )
		: CommandBase{ device }
		, m_pcb{ std::move( pcb ) }
	{
	}

	void PushConstantsCommand::apply( Context & context )const
	{
		// v13.39: no cache invalidation here. The push-constant buffer object stays the same;
		// only its contents change (Map/DISCARD), so the (stage, slot) -> buffer binding in the
		// state cache stays valid. Invalidating per push made the cache useless (one push per draw).
		if ( m_pcb.ubo )
		{
			auto buffer = get( m_pcb.ubo )->getBuffer();
			// v13.5 fix: WRITE_DISCARD hands back undefined memory, so writing only
			// [offset, offset+size) erased every earlier vkCmdPushConstants range.
			// Vulkan keeps the other ranges, so merge into a CPU shadow of the whole
			// block and upload all of it. (RimWorld pushes scale and offset separately;
			// the lost scale collapsed all UI vertices to one point -> black screen.)
			// v13.63: the shadow lives in the Buffer object (was a locked global map keyed by the
			// D3D11 pointer plus a GetPrivateData identity check on every push: ~3 us per push,
			// ~300 pushes per large command buffer, almost all of them unchanged).
			auto & entry = get( m_pcb.ubo )->getPushShadow();
			auto & shadow = entry.bytes;
			auto const end = size_t( m_pcb.data.offset ) + m_pcb.data.size;
			// v13.52: the recorder re-pushes every push-constant range after each pipeline
			// bind, mostly with unchanged bytes. When the buffer already holds exactly these
			// bytes, the Map(DISCARD)/copy/Unmap is skipped (the bridge is the only writer).
			bool const unchanged = pushconst::skipSame() && entry.uploaded && shadow.size() >= end
				&& std::memcmp( shadow.data() + m_pcb.data.offset, m_pcb.data.data.data(), m_pcb.data.size ) == 0;
			if ( shadow.size() < end )
			{
				shadow.resize( end, 0u );
			}
			std::memcpy( shadow.data() + m_pcb.data.offset
				, m_pcb.data.data.data()
				, m_pcb.data.size );
			if ( unchanged )
			{
				g_pushSkipped.fetch_add( 1u, std::memory_order_relaxed );
			}
			else
			{
			g_pushUploads.fetch_add( 1u, std::memory_order_relaxed );
			D3D11_MAPPED_SUBRESOURCE mapped;
			auto hr = context.context->Map( buffer
				, 0u
				, D3D11_MAP_WRITE_DISCARD
				, 0u
				, &mapped );
			checkError( context.device, hr, "Map" );
			if ( SUCCEEDED( hr ) && mapped.pData )
			{
				if ( !entry.byteWidth )   // v13.69: ask the driver once per buffer
				{
					D3D11_BUFFER_DESC desc{};
					buffer->GetDesc( &desc );
					entry.byteWidth = desc.ByteWidth;
				}
				size_t const copy = std::min( shadow.size(), size_t( entry.byteWidth ) );
				std::memcpy( mapped.pData, shadow.data(), copy );
				if ( copy < entry.byteWidth ) std::memset( static_cast< uint8_t * >( mapped.pData ) + copy, 0, entry.byteWidth - copy );
				context.context->Unmap( buffer, 0u );
				entry.uploaded = true;
			}
			else
			{
				entry.uploaded = false;
			}
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_VERTEX_BIT ) )
			{
				if ( context.stateCache.needsSet( statecache::Stage::vertex, statecache::Kind::constantBuffer, m_pcb.location, buffer ) )
					context.context->VSSetConstantBuffers( m_pcb.location, 1u, &buffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ) )
			{
				if ( context.stateCache.needsSet( statecache::Stage::hull, statecache::Kind::constantBuffer, m_pcb.location, buffer ) )
					context.context->HSSetConstantBuffers( m_pcb.location, 1u, &buffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT ) )
			{
				if ( context.stateCache.needsSet( statecache::Stage::domain, statecache::Kind::constantBuffer, m_pcb.location, buffer ) )
					context.context->DSSetConstantBuffers( m_pcb.location, 1u, &buffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_GEOMETRY_BIT ) )
			{
				if ( context.stateCache.needsSet( statecache::Stage::geometry, statecache::Kind::constantBuffer, m_pcb.location, buffer ) )
					context.context->GSSetConstantBuffers( m_pcb.location, 1u, &buffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_FRAGMENT_BIT ) )
			{
				if ( context.stateCache.needsSet( statecache::Stage::pixel, statecache::Kind::constantBuffer, m_pcb.location, buffer ) )
					context.context->PSSetConstantBuffers( m_pcb.location, 1u, &buffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_COMPUTE_BIT ) )
			{
				if ( context.stateCache.needsSet( statecache::Stage::compute, statecache::Kind::constantBuffer, m_pcb.location, buffer ) )
					context.context->CSSetConstantBuffers( m_pcb.location, 1u, &buffer );
			}
		}
	}

	uint64_t PushConstantsCommand::removeKey()const
	{
		// remove() nulls constant-buffer slot m_pcb.location in the stages of stageFlags.
		auto key = removeKeyMix( removeKeyStart( 2u ), uint64_t( reinterpret_cast< uintptr_t >( m_pcb.ubo ) ) );
		key = removeKeyMix( key, m_pcb.location );
		return removeKeyMix( key, uint64_t( m_pcb.data.stageFlags ) );
	}

	void PushConstantsCommand::remove( Context const & context )const
	{
		const_cast< Context & >( context ).stateCache.invalidate();
		if ( m_pcb.ubo )
		{
			static ID3D11Buffer * nullBuffer{ nullptr };

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_VERTEX_BIT ) )
			{
				context.context->VSSetConstantBuffers( m_pcb.location, 1u, &nullBuffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ) )
			{
				context.context->HSSetConstantBuffers( m_pcb.location, 1u, &nullBuffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT ) )
			{
				context.context->DSSetConstantBuffers( m_pcb.location, 1u, &nullBuffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_GEOMETRY_BIT ) )
			{
				context.context->GSSetConstantBuffers( m_pcb.location, 1u, &nullBuffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_FRAGMENT_BIT ) )
			{
				context.context->PSSetConstantBuffers( m_pcb.location, 1u, &nullBuffer );
			}

			if ( checkFlag( m_pcb.data.stageFlags, VK_SHADER_STAGE_COMPUTE_BIT ) )
			{
				context.context->CSSetConstantBuffers( m_pcb.location, 1u, &nullBuffer );
			}
		}
	}

	CommandPtr PushConstantsCommand::clone()const
	{
		return std::make_unique< PushConstantsCommand >( *this );
	}
}
