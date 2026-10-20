/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11CopyImageToBufferCommand.hpp"
#include "Command/Commands/D3D11CopyBufferCommand.hpp"
#include "Command/Commands/D3D11CopyImageCommand.hpp"

#include "Buffer/D3D11Buffer.hpp"
#include "Core/D3D11Device.hpp"
#include "Image/D3D11Image.hpp"
#include "Image/D3D11ImageView.hpp"
#include "Miscellaneous/D3D11RenderDiagnostics.hpp"

#include "ashesd3d11_api.hpp"

#include <ashes/common/Exception.hpp>

namespace ashes::D3D11_NAMESPACE
{
	namespace copyimgtobuf
	{
		// v12: a decoded BC image holds RGBA8 texels; its original blocks cannot be
		// reproduced, so vkCmdCopyImageToBuffer fails deterministically instead of
		// writing wrong bytes into the destination buffer.
		static void checkDecodedSource( VkImage src )
		{
			if ( !get( src )->isDecodedBc() ) return;
			uint32_t const key[] = { uint32_t( get( src )->getFormat() ), get( src )->getDimensions().width, get( src )->getDimensions().height };
			rdiag::report( "bc-decoded-readback-rejected", rdiag::hashBytes( 0x5249u, key, sizeof( key ) )
				, "src=%u:%ux%u representation=decoded-rgba8 result=VK_ERROR_FORMAT_NOT_SUPPORTED", key[0], key[1], key[2] );
			throw Exception{ VK_ERROR_FORMAT_NOT_SUPPORTED, "vkCmdCopyImageToBuffer from a decoded BC image" };
		}

		static uint32_t getBufferRowPitch( VkBufferImageCopy const & copyInfo )
		{
			return ( copyInfo.bufferRowLength
				? copyInfo.bufferRowLength
				: copyInfo.imageExtent.width );
		}

		static uint32_t getBufferHeightPitch( VkBufferImageCopy const & copyInfo )
		{
			return ( copyInfo.bufferImageHeight
				? copyInfo.bufferImageHeight
				: copyInfo.imageExtent.height );
		}

		static uint32_t getBufferDepthPitch( VkBufferImageCopy const & copyInfo )
		{
			return copyInfo.imageExtent.depth;
		}

		static std::vector< D3D11_BOX > doGetDstBoxes( VkImage image
			, VkBuffer buffer
			, ArrayView< VkBufferImageCopy const > const & copyInfos )
		{
			std::vector< D3D11_BOX > result;

			for ( auto & copyInfo : copyInfos )
			{
					auto size = getSize( VkExtent3D
						{
							std::max( 1u, getBufferRowPitch( copyInfo ) ),
							std::max( 1u, getBufferHeightPitch( copyInfo ) ),
							std::max( 1u, getBufferDepthPitch( copyInfo ) )
						}
							, get( image )->getFormat() ) * copyInfo.imageSubresource.layerCount;
				auto begin = get( buffer )->getMemoryOffset() + copyInfo.bufferOffset;
				result.push_back(
					{
						UINT( begin ),
						0u,
						0u,
						UINT( begin + size ),
						1u,
						1u
					}
				);
			}

			return result;
		}

		static std::vector< VkSubresourceLayout > doGetSrcLayouts( VkDevice device
			, VkImage image
			, ArrayView< VkBufferImageCopy const > const & copyInfos )
		{
			std::vector< VkSubresourceLayout > result;
			result.reserve( copyInfos.size() );

			for ( auto & copyInfo : copyInfos )
			{
				result.push_back( {} );
				get( device )->getImageSubresourceLayout( image
					, VkImageSubresource
					{
						copyInfo.imageSubresource.aspectMask,
						copyInfo.imageSubresource.mipLevel,
						copyInfo.imageSubresource.baseArrayLayer,
					}
				, result.back() );
			}

			return result;
		}

		static VkExtent3D getTexelBlockExtent( VkFormat format )
		{
			VkExtent3D texelBlockExtent{ 1u, 1u, 1u };

			if ( ashes::isCompressedFormat( format ) )
			{
				auto extent = ashes::getMinimalExtent2D( format );
				texelBlockExtent.width = extent.width;
				texelBlockExtent.height = extent.height;
			}
			else
			{
				texelBlockExtent.width = 1u;
			}

			return texelBlockExtent;
		}

		static uint32_t getTexelBlockByteSize( VkExtent3D const & texelBlockExtent
			, VkFormat format )
		{
			VkDeviceSize texelBlockSize;

			if ( !isDepthStencilFormat( format ) )
			{
				texelBlockSize = getSize( texelBlockExtent, format );
			}
			else
			{
				texelBlockSize = texelBlockExtent.width;
			}

			return uint32_t( texelBlockSize );
		}

		static uint32_t getBlockCount( uint32_t value, uint32_t blockExtent )
		{
			return std::max( 1u, ( value + blockExtent - 1u ) / blockExtent );
		}

		static void doCopyMapped( VkFormat format
			, VkBufferImageCopy const & copyInfo
			, uint8_t const * srcBuffer
			, VkSubresourceLayout const & srcLayout
			, uint8_t * dstBuffer )
		{
			auto const blockExtent = getTexelBlockExtent( format );
			auto const blockByteSize = getTexelBlockByteSize( blockExtent, format );
			auto const widthBlocks = getBlockCount( copyInfo.imageExtent.width, blockExtent.width );
			auto const heightBlocks = getBlockCount( copyInfo.imageExtent.height, blockExtent.height );
			auto const depthBlocks = getBlockCount( copyInfo.imageExtent.depth, blockExtent.depth );
			auto const bufferRowBlocks = getBlockCount( getBufferRowPitch( copyInfo ), blockExtent.width );
			auto const bufferHeightBlocks = getBlockCount( getBufferHeightPitch( copyInfo ), blockExtent.height );
			auto const copyRowBytes = VkDeviceSize( widthBlocks ) * blockByteSize;
			auto const dstRowPitch = VkDeviceSize( bufferRowBlocks ) * blockByteSize;
			auto const dstDepthPitch = dstRowPitch * bufferHeightBlocks;
			auto const dstLayerPitch = dstDepthPitch * depthBlocks;
			auto const srcX = VkDeviceSize( uint32_t( copyInfo.imageOffset.x ) / blockExtent.width ) * blockByteSize;
			auto const srcY = uint32_t( copyInfo.imageOffset.y ) / blockExtent.height;
			auto const srcZ = uint32_t( copyInfo.imageOffset.z ) / blockExtent.depth;
			auto const rowBytes = std::min( copyRowBytes
				, srcX < srcLayout.rowPitch
					? srcLayout.rowPitch - srcX
					: VkDeviceSize{} );

			if ( !srcBuffer || !dstBuffer || !rowBytes )
			{
				return;
			}

			auto srcData = srcBuffer
				+ srcZ * srcLayout.depthPitch
				+ srcY * srcLayout.rowPitch
				+ srcX;

			for ( auto l = 0u; l < copyInfo.imageSubresource.layerCount; ++l )
			{
				auto srcLayer = srcData + l * srcLayout.arrayPitch;
				auto dstLayer = dstBuffer + l * dstLayerPitch;

				for ( auto z = 0u; z < depthBlocks; ++z )
				{
					auto srcSlice = srcLayer + z * srcLayout.depthPitch;
					auto dstSlice = dstLayer + z * dstDepthPitch;

					for ( auto y = 0u; y < heightBlocks; ++y )
					{
						std::memcpy( dstSlice + y * dstRowPitch
							, srcSlice + y * srcLayout.rowPitch
							, size_t( rowBytes ) );
					}
				}
			}
		}

		static VkBuffer getStagingBuffer( VkDevice device
			, uint32_t size
			, VkDeviceMemory & memory )
		{
			VkBuffer result;
			allocate( result
				, get( device )->getAllocationCallbacks()
				, device
				, VkBufferCreateInfo
				{
					VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					nullptr,
					0u,
					size,
					VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					VK_SHARING_MODE_EXCLUSIVE,
					0u,
					nullptr,
				} );
			auto requirements = get( result )->getMemoryRequirements();
			uint32_t deduced = deduceMemoryType( requirements.memoryTypeBits
				, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT );
			allocate( memory
				, get( device )->getAllocationCallbacks()
				, device
				, VkMemoryAllocateInfo
				{
					VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					nullptr,
					requirements.size,
					deduced
				} );
			get( result )->bindMemory( memory, 0u );
			return result;
		}

		static VkImage getStagingTexture( VkDevice device
			, VkImage image
			, VkExtent3D dimensions
			, VkDeviceMemory & memory )
		{
			VkImage result;
			allocate( result
				, get( device )->getAllocationCallbacks()
				, device
				, VkImageCreateInfo
				{
					VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
					nullptr,
					0u,
					get( image )->getType(),
					get( image )->getFormat(),
					VkExtent3D
					{
						dimensions.width,
						dimensions.height,
						dimensions.depth,
					},
					1u,
					get( image )->getLayerCount(),
					VK_SAMPLE_COUNT_1_BIT,
					VK_IMAGE_TILING_LINEAR,
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
					VK_SHARING_MODE_EXCLUSIVE,
					0u,
					nullptr,
					VK_IMAGE_LAYOUT_UNDEFINED,
				} );
			auto requirements = get( result )->getMemoryRequirements();
			uint32_t deduced = deduceMemoryType( requirements.memoryTypeBits
				, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT );
			allocate( memory
				, get( device )->getAllocationCallbacks()
				, device
				, VkMemoryAllocateInfo
				{
					VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					nullptr,
					requirements.size,
					deduced
				} );
			get( result )->bindMemory( memory, 0u );
			return result;
		}

		static bool isMappable( D3D11_USAGE usage, UINT cpuAccessFlags, UINT requiredAccess )
		{
			return usage == D3D11_USAGE_STAGING
				&& ( cpuAccessFlags & requiredAccess ) == requiredAccess;
		}

		static bool isMappableForRead( VkImage image )
		{
			switch ( get( image )->getType() )
			{
			case VK_IMAGE_TYPE_1D:
				{
					D3D11_TEXTURE1D_DESC desc{};
					get( image )->getTexture1D()->GetDesc( &desc );
					return isMappable( desc.Usage, desc.CPUAccessFlags, D3D11_CPU_ACCESS_READ );
				}
			case VK_IMAGE_TYPE_2D:
				{
					D3D11_TEXTURE2D_DESC desc{};
					get( image )->getTexture2D()->GetDesc( &desc );
					return isMappable( desc.Usage, desc.CPUAccessFlags, D3D11_CPU_ACCESS_READ );
				}
			case VK_IMAGE_TYPE_3D:
				{
					D3D11_TEXTURE3D_DESC desc{};
					get( image )->getTexture3D()->GetDesc( &desc );
					return isMappable( desc.Usage, desc.CPUAccessFlags, D3D11_CPU_ACCESS_READ );
				}
			default:
				return false;
			}
		}

		static bool isMappableForWrite( VkBuffer buffer )
		{
			D3D11_BUFFER_DESC desc{};
			get( buffer )->getBuffer()->GetDesc( &desc );
			return isMappable( desc.Usage, desc.CPUAccessFlags, D3D11_CPU_ACCESS_WRITE );
		}
	}

	CopyImageToBufferCommand::CopyImageToBufferCommand( VkDevice device
		, ArrayView< VkBufferImageCopy const > const & copyInfo
		, VkImage src
		, VkBuffer dst )
		: CommandBase{ device }
		, m_src{ src }
		, m_dst{ dst }
			, m_copyInfo{ copyInfo.begin(), copyInfo.end() }
			, m_format{ getSRVFormat( get( m_src )->getFormat() ) }
			, m_srcLayouts{ copyimgtobuf::doGetSrcLayouts( device, m_src, copyInfo ) }
			, m_dstBoxes{ copyimgtobuf::doGetDstBoxes( m_src, m_dst, copyInfo ) }
			, m_srcMappable{ copyimgtobuf::isMappableForRead( m_src ) }
			, m_dstMappable{ copyimgtobuf::isMappableForWrite( m_dst ) }
	{
		copyimgtobuf::checkDecodedSource( m_src );
	}

	void CopyImageToBufferCommand::apply( Context & context )const
	{
		if ( m_dst ) ++get( m_dst )->getObjectMemory().contentVersion;   // v13.74: index pool
		for ( auto i = 0u; i < m_copyInfo.size(); ++i )
		{
			applyOne( context
				, m_copyInfo[i]
				, m_srcLayouts[i]
				, m_dstBoxes[i] );
		}
	}

	void CopyImageToBufferCommand::applyOne( Context & context
		, VkBufferImageCopy const & copyInfo
		, VkSubresourceLayout const & srcLayout
		, D3D11_BOX const & dstBox )const
	{
		VkImage stagingSrc{};
		VkBuffer stagingDst{};
		VkDeviceMemory stagingSrcMemory{};
			VkDeviceMemory stagingDstMemory{};
			VkImage const * src = &m_src;
			VkBuffer const * dst = &m_dst;
			VkBufferImageCopy mapCopyInfo{ copyInfo };
			VkSubresourceLayout mapSrcLayout{ srcLayout };
			D3D11_BOX mapDstBox{ dstBox };
			mapSrcLayout.offset += get( m_src )->getMemoryOffset();

		if ( !m_srcMappable )
		{
			stagingSrc = copyimgtobuf::getStagingTexture( getDevice()
				, m_src
				, copyInfo.imageExtent
				, stagingSrcMemory );
				doCopyToStaging( context
				, copyInfo
				, m_src
					, stagingSrc );
				src = &stagingSrc;
				mapCopyInfo.imageOffset = {};
				mapCopyInfo.imageSubresource.mipLevel = 0u;
				mapCopyInfo.imageSubresource.baseArrayLayer = 0u;
				get( getDevice() )->getImageSubresourceLayout( stagingSrc
					, VkImageSubresource{ copyInfo.imageSubresource.aspectMask, 0u, 0u }
					, mapSrcLayout );
			}

		if ( !m_dstMappable )
		{
			stagingDst = copyimgtobuf::getStagingBuffer( getDevice()
					, dstBox.right - dstBox.left
					, stagingDstMemory );
				dst = &stagingDst;
				mapDstBox = { 0u, 0u, 0u, dstBox.right - dstBox.left, 1u, 1u };
			}

			doMapCopy( context
				, mapCopyInfo
				, mapSrcLayout
				, mapDstBox
			, get( *src )->getFormat()
			, get( *src )->getMemory()
			, get( *dst )->getMemory() );

		if ( !m_dstMappable )
		{
			doCopyFromStaging( context
				, copyInfo
				, stagingDst
				, m_dst
				, dstBox );
		}

		if ( stagingDst )
		{
			deallocate( stagingDstMemory, get( getDevice() )->getAllocationCallbacks() );
			deallocate( stagingDst, get( getDevice() )->getAllocationCallbacks() );
		}

		if ( stagingSrc )
		{
			deallocate( stagingSrcMemory, get( getDevice() )->getAllocationCallbacks() );
			deallocate( stagingSrc, get( getDevice() )->getAllocationCallbacks() );
		}
	}

	CommandPtr CopyImageToBufferCommand::clone()const
	{
		return std::make_unique< CopyImageToBufferCommand >( *this );
	}

	void CopyImageToBufferCommand::doMapCopy( Context const & context
		, VkBufferImageCopy const & copyInfo
		, VkSubresourceLayout const & srcLayout
		, D3D11_BOX const & dstBox
		, VkFormat format
		, VkDeviceMemory src
		, VkDeviceMemory dst )const
		{
			if ( uint8_t * srcBuffer{};
				VK_SUCCESS == get( src )->lock( srcLayout.offset
					, srcLayout.size
					, D3D11CalcSubresource( copyInfo.imageSubresource.mipLevel
						, copyInfo.imageSubresource.baseArrayLayer
						, get( m_src )->getMipmapLevels() )
					, reinterpret_cast< void ** >( &srcBuffer ) ) )
			{
				get( src )->invalidate( context.context
					, srcLayout.offset
					, srcLayout.size );

				if ( uint8_t * dstBuffer{};
				VK_SUCCESS == get( dst )->lock( dstBox.left
					, VkDeviceSize( dstBox.right ) - dstBox.left
					, 0u
					, reinterpret_cast< void ** >( &dstBuffer ) ) )
			{
				copyimgtobuf::doCopyMapped( format
					, copyInfo
					, srcBuffer
					, srcLayout
					, dstBuffer );
				get( dst )->flush( context.context
					, dstBox.left
					, VkDeviceSize( dstBox.right ) - dstBox.left );
					get( dst )->unlock( context.context );
				}

				get( src )->unlock( context.context );
			}
	}

	void CopyImageToBufferCommand::doCopyToStaging( Context & context
		, VkBufferImageCopy const & copyInfo
		, VkImage src
		, VkImage staging )const
	{
		VkImageSubresourceLayers stagingSurbresouce{ copyInfo.imageSubresource };
		stagingSurbresouce.mipLevel = 0u;
		stagingSurbresouce.baseArrayLayer = 0u;
		CopyImageCommand command{ getDevice()
			, VkImageCopy
			{
				copyInfo.imageSubresource,
				copyInfo.imageOffset,
				stagingSurbresouce,
				VkOffset3D{},
				copyInfo.imageExtent,
			}
			, src
			, staging };
		command.apply( context );
	}

	void CopyImageToBufferCommand::doCopyFromStaging( Context & context
		, VkBufferImageCopy const & copyInfo
		, VkBuffer staging
		, VkBuffer dst
		, D3D11_BOX const & dstBox )const
	{
		CopyBufferCommand command{ getDevice()
			, VkBufferCopy
			{
				0u,
				copyInfo.bufferOffset,
				dstBox.right - dstBox.left
			}
			, staging
			, dst };
		command.apply( context );
	}
}
