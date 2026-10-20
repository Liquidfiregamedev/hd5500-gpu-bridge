/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11UploadMemoryCommand.hpp"

#include "Miscellaneous/D3D11DeviceMemory.hpp"

#include "Miscellaneous/D3D11EnvFlags.hpp"
#include "ashesd3d11_api.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>

namespace ashes::D3D11_NAMESPACE
{
	UploadMemoryCommand::UploadMemoryCommand( VkDevice device
		, ObjectMemory const * memory
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource
		, bool mappedOnly )
		: CommandBase{ device }
		, m_memory{ memory }
		, m_range{ offset, size }
		, m_subresource{ subresource }
		, m_mappedOnly{ mappedOnly }
		, m_parentLifetime{ get( memory->deviceMemory )->getLifetimeToken() }
	{
	}

	void UploadMemoryCommand::apply( Context & context )const
	{
		// ObjectMemory metadata belongs to its parent allocation, not to this command.
		if ( m_parentLifetime.expired() || !m_memory->resource ) return;
		auto parent = get( m_memory->deviceMemory );
		if ( m_mappedOnly )
		{
			if ( envflags::coherentRefreshOff() || !parent->isMapped() ) return;
		}
		// v13.24: skip a range this execution already refreshed (Waylith binds the same
		// host-visible vertex/index/storage buffers for hundreds of draws per frame, and
		// every buffer copy re-uploaded the whole staging buffer).
		bool const dedupe = envflags::uploadDedupe();
		uint64_t const begin = m_range.first;
		uint64_t const end = ( m_range.second == VK_WHOLE_SIZE || m_range.second > m_memory->size )
			? std::max< uint64_t >( m_memory->size, begin )
			: begin + m_range.second;
		if ( dedupe )
		{
			for ( auto const & done : context.uploaded )
			{
				if ( done.memory == m_memory && done.subresource == m_subresource
					&& done.begin <= begin && done.end >= end )
				{
					++context.uploadsSkipped;
					return;
				}
			}
		}
		parent->updateUpload( context.context
			, *m_memory
			, m_range.first
			, m_range.second
			, m_subresource );
		if ( dedupe )
		{
			context.uploaded.push_back( Context::UploadedRange{ m_memory, begin, end, m_subresource } );
		}
		++context.uploadsDone;
		context.uploadBytes += end - begin;
		if ( m_mappedOnly )
		{
			if ( envflags::uiTrace() )
			{
				static std::atomic< uint64_t > count{ 0u };
				auto serial = ++count;
				if ( serial <= 32u || ( serial & ( serial - 1u ) ) == 0u )
					std::cerr << "[RW-GPU-COHERENT] serial=" << serial
						<< " resource=" << m_memory->resource << " childBytes=" << m_memory->size
						<< " parentOffset=" << m_memory->offset << " offset=" << m_range.first
						<< " range=" << m_range.second << " mapped=" << parent->isMapped() << std::endl;
			}
		}
	}

	CommandPtr UploadMemoryCommand::clone()const
	{
		return std::make_unique< UploadMemoryCommand >( *this );
	}
}
