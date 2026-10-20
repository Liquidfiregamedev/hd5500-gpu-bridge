/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

#include <atomic>

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11WatchedBytes.hpp"

#include <cassert>
#include <memory>

namespace ashes::D3D11_NAMESPACE
{
	class DeviceContextLock;

	class ObjectMemory
	{
		friend class DeviceMemory;

	public:
		ObjectMemory( ObjectMemory const & ) = delete;
		ObjectMemory & operator=( ObjectMemory const & ) = delete;
		ObjectMemory( VkDevice device
			, VkDeviceMemory deviceMemory
			, ID3D11Resource * resource
			, VkDeviceSize offset
			, VkMemoryAllocateInfo allocateInfo
			, D3D11_MAP mapFlags );
			ObjectMemory( ObjectMemory && rhs )noexcept;
			ObjectMemory & operator=( ObjectMemory && rhs )noexcept;
			~ObjectMemory()noexcept;
			void releaseResource()noexcept;

			VkResult lock( ID3D11DeviceContext * context
				, UINT subresource
			, D3D11_MAPPED_SUBRESOURCE & data )const;
		void unlock( ID3D11DeviceContext * context
			, UINT subresource )const;

	private:
		void upload( DeviceContextLock const & context
			, uint8_t const * data
			, UINT subresource
			, VkDeviceSize offset
			, VkDeviceSize size )const;
		void download( DeviceContextLock const & context
			, uint8_t * data
			, UINT subresource
			, VkDeviceSize offset
			, VkDeviceSize size )const;

	public:
		VkDevice device{};
		VkDeviceMemory deviceMemory{};
		ID3D11Resource * resource{};
		VkDeviceSize offset{};
		VkDeviceSize size{};
		VkMemoryAllocateInfo allocateInfo{};
		D3D11_MAP mapFlags{};
		UINT mipPrefix{};
		// v12: logical BC format when the native texture is a decoded RGBA8 copy
		// (VK_FORMAT_UNDEFINED otherwise), and accounted native texture bytes.
		VkFormat decodedFrom{ VK_FORMAT_UNDEFINED };
		uint64_t nativeTextureBytes{};
		mutable ID3D11Resource * mapStaging{};
		std::vector< D3D11_SUBRESOURCE_DATA > subresources{};
		mutable bool everUploaded{};   // v13.48: first upload is always complete
		mutable std::atomic< bool > gpuWritten{};     // v13.74 (v13.76 atomic: written at record time, read on the submit thread): was a copy/fill/update destination (never served from the uniform ring)
		// v13.74: index pool (D3D11IndexPool.hpp). serial = identity that is never reused (addresses are);
		// contentVersion changes whenever the bridge writes this object's D3D11 resource.
		static uint64_t nextSerial()
		{
			static std::atomic< uint64_t > counter{ 0u };
			return ++counter;
		}
		uint64_t serial{ nextSerial() };
		mutable std::atomic< uint64_t > contentVersion{ 1u };   // v13.76: atomic (bumped by flush/unmap on the app thread)
		mutable int8_t pureConstantBuffer{ -1 };   // v13.76: cached "D3D11 buffer is a pure constant buffer" (-1 = not checked)
	};
	using ObjectMemoryPtr = std::unique_ptr< ObjectMemory >;
	/**
	*\brief
	*	Class wrapping a storage allocated to a data buffer.
	*/
	class DeviceMemory
	{
	public:
		DeviceMemory( VkDevice device
			, VkMemoryAllocateInfo allocateInfo );
		~DeviceMemory()noexcept;
		VkResult bindToBuffer( VkBuffer buffer
			, VkDeviceSize memoryOffset
			, ObjectMemory *& objectMemory );
		VkResult bindToImage1D( VkImage image
			, VkDeviceSize memoryOffset
			, ObjectMemory *& objectMemory );
		VkResult bindToImage2D( VkImage image
			, VkDeviceSize memoryOffset
			, ObjectMemory *& objectMemory );
		VkResult bindToImage3D( VkImage image
			, VkDeviceSize memoryOffset
			, ObjectMemory *& objectMemory );

		void updateUpload( DeviceContextLock const & context
			, ObjectMemory const & memory
			, VkDeviceSize offset
			, VkDeviceSize size
			, UINT subresource )const;
		void updateDownload( DeviceContextLock const & context
			, ObjectMemory const & memory
			, VkDeviceSize offset
			, VkDeviceSize size
			, UINT subresource )const;
		void updateUpload( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size
			, UINT subresource )const;
		void updateDownload( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size
			, UINT subresource )const;
		void updateData( VkDeviceMemory src
			, VkDeviceSize srcOffset
			, VkDeviceSize dstOffset
			, VkDeviceSize size );

		VkResult lock( VkDeviceSize offset
			, VkDeviceSize size
			, VkMemoryMapFlags flags // Effectively expects the D3D11 Subresource index
			, void ** data )const;
		VkResult flush( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size )const;
		VkResult invalidate( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size )const;
		void unlock( DeviceContextLock const & context )const;

		bool isMapped()const
		{
			return m_mapped;
		}
		// v13.30: read-only view of the CPU shadow bytes, for the vertex compare diagnostic.
		uint8_t const * debugShadow()const
		{
			return m_data.empty() ? nullptr : m_data.data();
		}
		uint8_t const * shadow()const   // v13.74: uniform ring source
		{
			return m_data.empty() ? nullptr : m_data.data();
		}
		VkDeviceSize debugShadowSize()const
		{
			return VkDeviceSize( m_data.size() );
		}

		void upload( DeviceContextLock const & context )const
		{
			upload( context, m_mappedOffset, m_mappedSize );
		}

		void download( DeviceContextLock const & context )const
		{
			download( context, m_mappedOffset, m_mappedSize );
		}

		VkMemoryPropertyFlags getMemoryPropertyFlags()const
		{
			return m_propertyFlags;
		}

		VkDevice getDevice()const
		{
			return m_device;
		}
		std::weak_ptr< uint8_t > getLifetimeToken()const
		{
			return m_lifetime;
		}

	public:
		DeviceMemoryDestroySignal onDestroy;

	private:
		void upload( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size
			, UINT subresource )const;
		void download( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size
			, UINT subresource )const;

		void upload( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size )const
		{
			upload( context, offset, size, m_mappedSubresource );
		}

		void download( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size )const
		{
			download( context, offset, size, m_mappedSubresource );
		}

	private:
		VkDevice m_device{};
		VkMemoryAllocateInfo m_allocateInfo{};
		VkMemoryPropertyFlags m_propertyFlags{};
		VkMemoryRequirements m_requirements{};
		std::vector< ObjectMemoryPtr > m_objects{};
		std::shared_ptr< uint8_t > m_lifetime{ std::make_shared< uint8_t >( 0u ) };
		mutable bool m_dirty{ true };
		mutable bool m_mapped{ false };
		mutable VkDeviceSize m_mappedOffset{ 0u };
		mutable VkDeviceSize m_mappedSize{ 0u };
		mutable UINT m_mappedSubresource{ 0u };
		mutable WatchedBytes m_data;   // v13.48: write-tracked shadow
	};
}
