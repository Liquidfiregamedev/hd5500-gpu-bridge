/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"

#include <renderer/RendererCommon/IcdObject.hpp>

namespace ashes::D3D11_NAMESPACE
{
	class Queue
		: public ashes::IcdObject
		, public NonCopyable
	{
	public:
		Queue( VkDevice device
			, VkDeviceQueueCreateInfo createInfo
			, uint32_t index );
		~Queue()noexcept;
		VkResult submit( ArrayView< VkSubmitInfo const > const & infos
			, VkFence fence )const;
		VkResult bindSparse()const;
		VkResult present( VkPresentInfoKHR const & presentInfo )const;
		VkResult waitIdle()const;
#if VK_EXT_debug_utils
		void beginDebugUtilsLabel( VkDebugUtilsLabelEXT const & labelInfo )const;
		void endDebugUtilsLabel()const;
		void insertDebugUtilsLabel( VkDebugUtilsLabelEXT const & labelInfo )const;
#endif
#if VK_EXT_debug_marker
		void debugMarkerBegin( VkDebugMarkerMarkerInfoEXT const & labelInfo )const;
		void debugMarkerEnd()const;
		void debugMarkerInsert( VkDebugMarkerMarkerInfoEXT const & labelInfo )const;
#endif

		inline VkDevice getDevice()const
		{
			return m_device;
		}

	private:
		VkResult doSubmit( Context & context
			, ArrayView< VkCommandBuffer const > const & commandBuffers )const;
		bool beginTiming( Context & context, LARGE_INTEGER cpuBegin )const;
		void endTiming( Context & context, LARGE_INTEGER cpuEnd )const;
		void pollTiming( Context & context )const;
		void releaseTiming()noexcept;

	private:
		VkDevice m_device;
		VkDeviceQueueCreateInfo m_createInfo;
		uint32_t m_index;
		ID3D11Query * m_waitIdleQuery{ nullptr };
		struct TimingSlot
		{
			ID3D11Query * disjoint{};
			ID3D11Query * begin{};
			ID3D11Query * end{};
			LARGE_INTEGER cpuBegin{};
			LARGE_INTEGER cpuEnd{};
			bool pending{};
		};
		mutable bool m_timingEnabled{};
		mutable uint32_t m_timingNext{};
		mutable TimingSlot * m_activeTiming{};
		mutable std::array< TimingSlot, 16u > m_timingSlots{};
		mutable std::vector< double > m_gpuTimingSamples;
		mutable std::vector< double > m_cpuTimingSamples;
		mutable uint64_t m_timingDropped{};
		mutable uint64_t m_timingDisjoint{};
		mutable Optional< DebugLabel > m_label;
	};
}
