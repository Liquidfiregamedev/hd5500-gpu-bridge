#include "Image/D3D11Image.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"

#include "Command/D3D11CommandBuffer.hpp"
#include "Command/D3D11Queue.hpp"
#include "Core/D3D11Device.hpp"
#include "Core/D3D11PhysicalDevice.hpp"
#include "Core/D3D11Instance.hpp"
#include "Miscellaneous/D3D11DeviceMemory.hpp"
#include "Command/D3D11Queue.hpp"
#include "Image/D3D11ImageView.hpp"

#include "ashesd3d11_api.hpp"
#include <atomic>
#include <cstdlib>
#include <ashes/common/Exception.hpp>
#include <cstdio>

namespace ashes::D3D11_NAMESPACE
{
	namespace
	{
		void releaseImageResource( ObjectMemory *& objectMemory
			, std::weak_ptr< uint8_t > const & memoryLifetime )noexcept
		{
			// DeviceMemory owns ObjectMemory. It may legally have been freed
			// before the image wrapper, in which case objectMemory is stale and
			// must not be dereferenced.
			if ( objectMemory && !memoryLifetime.expired() )
			{
				if ( objectMemory->resource )
				{
					static std::atomic_uint traceCount{};
					if ( envflags::imageLifetimeTrace()
						&& traceCount.fetch_add( 1u, std::memory_order_relaxed ) < 4096u )
					{
						std::fputs( "[RW-HD5500-IMAGE-LIFETIME] IMAGE_RESOURCE_RELEASE\n", stderr );
						std::fflush( stderr );
					}
				}
				// Keep the parent-owned metadata in DeviceMemory::m_objects so
				// recorded pointers remain stable; releaseResource is idempotent.
				objectMemory->releaseResource();
			}
			objectMemory = nullptr;
		}
	}

	Image::Image( Image && rhs )noexcept
		: m_device{ rhs.m_device }
		, m_createInfo{ std::move( rhs.m_createInfo ) }
		, m_image{ rhs.m_image }
		, m_memory{ rhs.m_memory }
		, m_memoryOffset{ rhs.m_memoryOffset }
		, m_objectMemory{ rhs.m_objectMemory }
		, m_memoryLifetime{ std::move( rhs.m_memoryLifetime ) }
		, m_debugName{ std::move( rhs.m_debugName ) }
		, m_memoryRequirements{ rhs.m_memoryRequirements }
	{
		rhs.m_device = nullptr;
		rhs.m_createInfo = {};
		rhs.m_image.resource = nullptr;
		rhs.m_memory = nullptr;
		rhs.m_memoryOffset = 0u;
		rhs.m_objectMemory = nullptr;
		rhs.m_memoryLifetime.reset();
		rhs.m_debugName.clear();
		rhs.m_memoryRequirements = {};
	}

	Image & Image::operator=( Image && rhs )noexcept
	{
		if ( this != &rhs )
		{
			releaseImageResource( m_objectMemory, m_memoryLifetime );
			m_image.resource = nullptr;
			m_memory = nullptr;
			m_memoryOffset = 0u;
			m_memoryLifetime.reset();

			m_device = rhs.m_device;
			m_createInfo = std::move( rhs.m_createInfo );
			m_image = rhs.m_image;
			m_memory = rhs.m_memory;
			m_memoryOffset = rhs.m_memoryOffset;
			m_objectMemory = rhs.m_objectMemory;
			m_memoryLifetime = std::move( rhs.m_memoryLifetime );
			m_debugName = std::move( rhs.m_debugName );
			m_memoryRequirements = rhs.m_memoryRequirements;

			rhs.m_device = nullptr;
			rhs.m_createInfo = {};
			rhs.m_image.resource = nullptr;
			rhs.m_memory = nullptr;
			rhs.m_memoryOffset = 0u;
			rhs.m_objectMemory = nullptr;
			rhs.m_memoryLifetime.reset();
			rhs.m_debugName.clear();
			rhs.m_memoryRequirements = {};
		}
		return *this;
	}

	Image::Image( VkDevice device
		, VkImageCreateInfo createInfo )
		: m_device{ device }
		, m_createInfo{ std::move( createInfo ) }
		, m_image{ nullptr }
	{
		if ( m_createInfo.format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK
			&& m_createInfo.format <= VK_FORMAT_BC7_SRGB_BLOCK
			&& !get( get( m_device )->getPhysicalDevice() )->getFeatures().textureCompressionBC )
		{
			std::fprintf( stderr, "[RW-GPU-TEX2D] BC_FORMAT_REJECT vkFormat=%u extent=%ux%ux%u mips=%u vkUsage=0x%X\n",
				unsigned( m_createInfo.format ), m_createInfo.extent.width, m_createInfo.extent.height,
				m_createInfo.extent.depth, m_createInfo.mipLevels, unsigned( m_createInfo.usage ) );
			std::fflush( stderr );
			throw Exception{ VK_ERROR_FORMAT_NOT_SUPPORTED, "BC textures disabled by the explicit GPU compatibility mode" };
		}
		// v13.11: Vulkan lets vkCmdClearColorImage target any TRANSFER_DST colour image;
		// D3D11 can only clear through a render-target view. Waylith clears freshly
		// created sampled textures (usage SRC|DST|SAMPLED, no attachment bit), which used
		// to be silently dropped. Promote such images to render targets when the running
		// driver can create an RTV for the format (asked, not assumed: this varies per GPU
		// generation). Host-visible / staging images are excluded later by getBindFlags.
		if ( m_createInfo.imageType == VK_IMAGE_TYPE_2D
			&& checkFlag( m_createInfo.usage, VK_IMAGE_USAGE_TRANSFER_DST_BIT )
			&& !ashes::D3D11_NAMESPACE::isRenderTarget( m_createInfo.usage )
			&& m_createInfo.samples == VK_SAMPLE_COUNT_1_BIT
			&& !isCompressedFormat( m_createInfo.format )
			&& !isDepthOrStencilFormat( m_createInfo.format ) )
		{
			auto const dxgi = getDxgiFormat( m_createInfo.format );
			UINT support = 0u;
			if ( dxgi != DXGI_FORMAT_UNKNOWN
				&& SUCCEEDED( get( m_device )->getDevice()->CheckFormatSupport( dxgi, &support ) )
				&& ( support & D3D11_FORMAT_SUPPORT_RENDER_TARGET ) )
			{
				m_createInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			}
		}
		doInitialiseMemoryRequirements();
	}

	Image::Image( VkDevice device
		, VkFormat format
		, VkExtent2D const & dimensions
		, ID3D11Texture2D * image )
		: Image{ device
			, { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				nullptr,
				0u,
				VK_IMAGE_TYPE_2D,
				format,
				VkExtent3D{ dimensions.width, dimensions.height, 1u },
				1u,
				1u,
				VK_SAMPLE_COUNT_1_BIT,
				VK_IMAGE_TILING_OPTIMAL,
				VkImageUsageFlags( isDepthOrStencilFormat( format )
					? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
					: VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ),
				VK_SHARING_MODE_EXCLUSIVE,
				0u,
				nullptr,
				VK_IMAGE_LAYOUT_UNDEFINED } }
	{
		m_image.tex2D = image;
	}

	Image::Image( VkDevice device
		, VkFormat format
		, VkExtent2D const & dimensions
		, VkImageUsageFlags usageFlags
		, VkImageTiling tiling )
		: Image{ device
			, { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				nullptr,
				0u,
				VK_IMAGE_TYPE_2D,
				format,
				VkExtent3D{ dimensions.width, dimensions.height, 1u },
				1u,
				1u,
				VK_SAMPLE_COUNT_1_BIT,
				tiling,
				usageFlags,
				VK_SHARING_MODE_EXCLUSIVE,
				0u,
				nullptr,
				VK_IMAGE_LAYOUT_UNDEFINED } }
		{
			doInitialiseMemoryRequirements();
		}

	Image::~Image()noexcept
	{
		releaseImageResource( m_objectMemory, m_memoryLifetime );
		m_image.resource = nullptr;
		m_memory = nullptr;
		m_memoryOffset = 0u;
		m_memoryLifetime.reset();
	}

	VkMemoryRequirements Image::getMemoryRequirements()const
	{
		return m_memoryRequirements;
	}

	std::vector< VkSparseImageMemoryRequirements > Image::getSparseImageMemoryRequirements()const
	{
		return {};
	}

	void Image::doInitialiseMemoryRequirements()
	{
		auto physicalDevice = get( get( m_device )->getPhysicalDevice() );
		auto extent = ashes::getMinimalExtent3D( getFormat() );
		m_memoryRequirements.alignment = ashes::getSize( extent, getFormat() );
		m_memoryRequirements.size = getTotalSize( getDimensions(), getFormat(), getLayerCount(), getMipmapLevels(), uint32_t( m_memoryRequirements.alignment ) );
		m_memoryRequirements.memoryTypeBits = physicalDevice->getMemoryTypeBits( VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
			, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT );
		m_memoryRequirements.size = ashes::getAlignedSize( m_memoryRequirements.size, m_memoryRequirements.alignment );
	}

	void Image::generateMipmaps( VkCommandBuffer commandBuffer )const
	{
		get( commandBuffer )->generateMipmaps( get( this ) );
	}

	VkResult Image::bindMemory( VkDeviceMemory memory
		, VkDeviceSize memoryOffset )
	{
		if ( get( m_device )->getStatus() != VK_SUCCESS )
		{
			return VK_ERROR_DEVICE_LOST;
		}

		// Do not publish a parent handle or lifetime token until the native
		// resource was created successfully. In particular, isMapped() must
		// never dereference a parent that was already freed.
		auto parentMemory = get( memory );
		ObjectMemory * objectMemory{};
		VkResult result = VK_SUCCESS;

		switch ( getType() )
		{
		case VK_IMAGE_TYPE_1D:
			result = parentMemory->bindToImage1D( get( this )
				, memoryOffset
				, objectMemory );
			break;
		case VK_IMAGE_TYPE_2D:
			result = parentMemory->bindToImage2D( get( this )
				, memoryOffset
				, objectMemory );
			break;
		case VK_IMAGE_TYPE_3D:
			result = parentMemory->bindToImage3D( get( this )
				, memoryOffset
				, objectMemory );
			break;
		default:
			result = VK_ERROR_INITIALIZATION_FAILED;
			break;
		}

		if ( result != VK_SUCCESS || !objectMemory || !objectMemory->resource )
		{
			return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
		}

		m_memory = memory;
		m_memoryOffset = memoryOffset;
		m_objectMemory = objectMemory;
		m_memoryLifetime = parentMemory->getLifetimeToken();

		if ( !m_debugName.empty() )
		{
			m_objectMemory->resource->SetPrivateData( WKPDID_D3DDebugObjectName
				, UINT( m_debugName.size() )
				, m_debugName.c_str() );
		}

		m_image.resource = m_objectMemory->resource;
		return result;
	}

	bool Image::isMapped()const
	{
		return m_memory
			&& m_objectMemory
			&& !m_memoryLifetime.expired()
			&& get( m_memory )->isMapped();
	}
}
