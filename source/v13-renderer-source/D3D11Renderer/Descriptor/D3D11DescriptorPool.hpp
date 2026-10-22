/**
*\file
*	DescriptorSetPool.h
*\author
*	Sylvain Doremus
*/
#ifndef ___D3D11Renderer_DescriptorPool_HPP___
#define ___D3D11Renderer_DescriptorPool_HPP___
#pragma once

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	class DescriptorPool
		: public NonCopyable
	{
	public:
		DescriptorPool( VkDevice device
			, VkDescriptorPoolCreateInfo createInfos );
		~DescriptorPool()noexcept;

		void registerSet( VkDescriptorSet set );
		VkResult reset();
		VkResult freeDescriptors( ArrayView< VkDescriptorSet const > sets );
		// v13.81: hands out a recycled set of this layout (nullptr when none).
		VkDescriptorSet tryReuse( VkDescriptorSetLayout layout );

		inline VkDevice getDevice()const
		{
			return m_device;
		}

	private:
		VkDevice m_device;
		VkDescriptorPoolSizeArray m_poolSizes;
		VkDescriptorPoolCreateInfo m_createInfos;
		// v13.81: sets as hash sets (vkFreeDescriptorSets was a linear search, and a set freed
		// individually stayed in m_allSets, so a later reset deleted it a second time).
		std::unordered_set< VkDescriptorSet > m_sets;
		std::unordered_set< VkDescriptorSet > m_allSets;
		// v13.81: sets released by reset/free, kept per layout and handed out again
		// instead of destroying and re-creating them (RW_GPU_SET_RECYCLE=0 = old behaviour).
		std::unordered_map< VkDescriptorSetLayout, std::vector< VkDescriptorSet > > m_free;
		void release( VkDescriptorSet set );
	};
}

#endif
