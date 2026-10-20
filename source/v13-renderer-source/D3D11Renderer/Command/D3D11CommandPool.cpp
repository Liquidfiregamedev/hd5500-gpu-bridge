/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/D3D11CommandPool.hpp"

#include "Command/D3D11CommandBuffer.hpp"
#include "Core/D3D11Device.hpp"

#include "ashesd3d11_api.hpp"

namespace ashes::D3D11_NAMESPACE
{
	CommandPool::CommandPool( VkDevice device
		, VkCommandPoolCreateInfo createInfo )
		: m_device{ device }
		, m_createInfo{ std::move( createInfo ) }
	{
	}

	CommandPool::~CommandPool()noexcept
	{
		while ( !m_commands.empty() )
		{
			auto command = m_commands.back();
			m_commands.pop_back();
			deallocateNA( command );
		}
	}

	void CommandPool::registerCommands( VkCommandBuffer commands )
	{
		m_commands.push_back( commands );
	}

	void CommandPool::detachCommands( VkCommandBuffer command )noexcept
	{
		auto it = std::find( m_commands.begin(), m_commands.end(), command );
		if ( it != m_commands.end() ) m_commands.erase( it );
	}

	VkResult CommandPool::reset()
	{
		// Vulkan pool reset keeps allocated command-buffer handles valid.
		auto commands = m_commands;
		for ( auto command : commands )
		{
			if ( std::find( m_commands.begin(), m_commands.end(), command ) != m_commands.end() )
			{
				auto result = get( command )->reset();
				if ( result != VK_SUCCESS ) return result;
			}
		}
		return VK_SUCCESS;
	}

	VkResult CommandPool::freeCommands( ArrayView< VkCommandBuffer const > commands )noexcept
	{
		for ( auto & command : commands )
		{
			if ( auto it = std::find( m_commands.begin()
					, m_commands.end()
					, command );
				it != m_commands.end() )
			{
				m_commands.erase( it );
				deallocateNA( command );
			}
		}

		return VK_SUCCESS;
	}
}
