/**
*\file
*	RenderingResources.h
*\author
*	Sylvain Doremus
*/
#ifndef ___D3D11Renderer_Buffer_HPP___
#define ___D3D11Renderer_Buffer_HPP___
#pragma once

#include "renderer/D3D11Renderer/Miscellaneous/D3D11DeviceMemory.hpp"

#include <map>
#include <utility>

namespace ashes::D3D11_NAMESPACE
{
	class Buffer
		: public NonCopyable
	{
	public:
		Buffer( VkDevice device
			, VkBufferCreateInfo createInfo );
		~Buffer()noexcept;
		VkResult bindMemory( VkDeviceMemory memory
			, VkDeviceSize memoryOffset );

		VkMemoryRequirements getMemoryRequirements()const;
		bool isMapped()const;
		void copyFrom( ID3D11DeviceContext * context
			, VkBuffer src
			, D3D11_BOX const & srcBox
			, UINT dstOffset )const;

		void setDebugName( std::string name )
		{
			m_debugName = std::move( name );
		}

		VkDeviceMemory getMemory()const
		{
			assert( m_memory != nullptr );
			return m_memory;
		}

		VkDeviceSize getMemoryOffset()const
		{
			return m_memoryOffset;
		}

		// Each native buffer contains this Vulkan buffer only, not its parent pool.
		// Parent-shadow accesses continue to use getMemoryOffset().
		VkDeviceSize getNativeOffset()const
		{
			return 0u;
		}

		ObjectMemory const & getObjectMemory()const
		{
			assert( m_objectMemory != nullptr );
			return *m_objectMemory;
		}

		ID3D11UnorderedAccessView * getUnorderedAccessView()const
		{
			return m_unorderedAccessView;
		}
		// v13.47b: raw UAV covering only [offset, offset+range) of the buffer, as the Vulkan
		// descriptor (VkDescriptorBufferInfo) asks. Ashes used to bind the whole buffer, so a
		// shader read a ring-buffer sub-allocation (Waylith's per-draw _InstanceTransforms)
		// from offset 0 instead of its own offset. Views are cached per (offset, range).
		ID3D11UnorderedAccessView * getUnorderedAccessView( VkDeviceSize offset, VkDeviceSize range )const;

		ID3D11Resource * getResource()const
		{
			return getObjectMemory().resource;
		}

		ID3D11Buffer * getBuffer()const
		{
			return &static_cast< ID3D11Buffer & >( *getResource() );
		}

		VkBufferUsageFlags getUsage()const
		{
			return m_createInfo.usage;
		}

		VkDeviceSize getSize()const
		{
			return m_createInfo.size;
		}

		VkDevice getDevice()const
		{
			return m_device;
		}

		// v13.63: CPU copy of a push-constant buffer's contents (Vulkan keeps ranges that a
		// vkCmdPushConstants call does not touch; D3D11 Map(DISCARD) does not). Lives with the
		// buffer object, so no global map, lock or private-data identity check per push.
		struct PushShadow
		{
			std::vector< uint8_t > bytes;
			bool uploaded{};   // the GPU buffer holds exactly `bytes`
			UINT byteWidth{};  // v13.69: cached D3D11 ByteWidth (0 = not yet asked)
		};
		PushShadow & getPushShadow()const noexcept
		{
			return m_pushShadow;
		}

	private:
		mutable PushShadow m_pushShadow;
		VkDevice m_device;
		VkBufferCreateInfo m_createInfo;
		VkDeviceMemory m_memory{};
		VkDeviceSize m_memoryOffset{ 0u };
		ObjectMemory * m_objectMemory{ nullptr };
		std::weak_ptr< uint8_t > m_memoryLifetime;
		ID3D11UnorderedAccessView * m_unorderedAccessView{ nullptr };
		mutable std::map< std::pair< VkDeviceSize, VkDeviceSize >, ID3D11UnorderedAccessView * > m_rangeUavs;
		void doClearRangeUavs()const noexcept;
		void doClearRangeUavsLocked()const noexcept;
		std::string m_debugName;
	};
}

#endif
