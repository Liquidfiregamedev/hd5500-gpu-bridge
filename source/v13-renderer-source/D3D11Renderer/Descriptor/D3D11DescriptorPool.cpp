#include "Descriptor/D3D11DescriptorPool.hpp"

#include "Descriptor/D3D11DescriptorSet.hpp"
#include "Descriptor/D3D11DescriptorSetLayout.hpp"
#include "Core/D3D11Device.hpp"

#include "ashesd3d11_api.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11EnvFlags.hpp"

#include <atomic>

namespace ashes::D3D11_NAMESPACE
{
	DescriptorPool::DescriptorPool( VkDevice device
		, VkDescriptorPoolCreateInfo createInfos )
		: m_device{ device }
		, m_poolSizes{ makeVector( createInfos.pPoolSizes, createInfos.poolSizeCount ) }
		, m_createInfos{ std::move( createInfos ) }
	{
		m_createInfos.pPoolSizes = m_poolSizes.data();
	}

	namespace setrecycle
	{
		std::atomic< uint64_t > g_reused{ 0u };
		std::atomic< uint64_t > g_created{ 0u };
		inline bool enabled()
		{
			static bool const result = envflags::on( "RW_GPU_SET_RECYCLE", true );
			return result;
		}
	}

	DescriptorPool::~DescriptorPool()noexcept
	{
		for ( auto & set : m_allSets )
		{
			deallocateNA( set );
		}
		for ( auto & [_, list] : m_free )
		{
			for ( auto & set : list )
			{
				deallocateNA( set );
			}
		}
	}

	void DescriptorPool::registerSet( VkDescriptorSet set )
	{
		if ( checkFlag( m_createInfos.flags, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT ) )
		{
			m_sets.insert( set );
		}

		m_allSets.insert( set );
	}

	void DescriptorPool::release( VkDescriptorSet set )
	{
		if ( setrecycle::enabled() )
		{
			auto & list = m_free[get( set )->getLayout()];
			if ( list.size() < 16384u && get( set )->recycle() )
			{
				list.push_back( set );
				return;
			}
		}
		deallocateNA( set );
	}

	VkDescriptorSet DescriptorPool::tryReuse( VkDescriptorSetLayout layout )
	{
		if ( !setrecycle::enabled() )
		{
			return VK_NULL_HANDLE;
		}
		auto it = m_free.find( layout );
		if ( it == m_free.end() || it->second.empty() )
		{
			setrecycle::g_created.fetch_add( 1u, std::memory_order_relaxed );
			return VK_NULL_HANDLE;
		}
		auto set = it->second.back();
		it->second.pop_back();
		registerSet( set );
		setrecycle::g_reused.fetch_add( 1u, std::memory_order_relaxed );
		return set;
	}

	VkResult DescriptorPool::reset()
	{
		for ( auto & set : m_allSets )
		{
			release( set );
		}

		m_allSets.clear();
		m_sets.clear();
		return VK_SUCCESS;
	}

	VkResult DescriptorPool::freeDescriptors( ArrayView< VkDescriptorSet const > sets )
	{
		if ( checkFlag( m_createInfos.flags, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT ) )
		{
			for ( auto set : sets )
			{
				if ( m_sets.erase( set ) )
				{
					m_allSets.erase( set );
					release( set );
				}
			}
		}

		return VK_SUCCESS;
	}
}
