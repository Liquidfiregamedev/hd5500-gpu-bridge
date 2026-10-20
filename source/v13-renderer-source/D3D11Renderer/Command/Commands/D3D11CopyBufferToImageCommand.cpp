/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include <cstdlib>
#include "Miscellaneous/D3D11EnvFlags.hpp"
#include <cstdio>
#include "Command/Commands/D3D11CopyBufferToImageCommand.hpp"

#include "Command/Commands/D3D11CopyBufferCommand.hpp"
#include "Command/Commands/D3D11CopyImageCommand.hpp"

#include "Buffer/D3D11Buffer.hpp"
#include "Image/D3D11Image.hpp"
#include "Image/D3D11ImageView.hpp"
#include "Miscellaneous/D3D11BcDecode.hpp"
#include "Miscellaneous/D3D11DeviceMemory.hpp"
#include "Miscellaneous/D3D11RenderDiagnostics.hpp"

#include "ashesd3d11_api.hpp"

#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	namespace copybuftoimg
	{
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
		
		static VkDeviceSize doGetBufferSize( VkFormat format
			, VkBufferImageCopy const & copyInfo )
		{
			VkExtent3D bufferPitch
			{
				std::max( 1u, getBufferRowPitch( copyInfo ) ),
				std::max( 1u, getBufferHeightPitch( copyInfo ) ),
				std::max( 1u, getBufferDepthPitch( copyInfo ) )
			};
			return getSize( bufferPitch, format ) * copyInfo.imageSubresource.layerCount;
		}

		static D3D11_BOX doGetSrcBox( VkFormat format
			, VkBufferImageCopy const & copyInfo
			, uint32_t slice )
		{
			auto begin = copyInfo.bufferOffset;
			auto end = begin + doGetBufferSize( format, copyInfo );
			return
			{
				UINT( begin ),    // left
				0u,               // top
				slice,            // front
				UINT( end ),      // right
				1u,               // bottom
				slice + 1u        // back
			};
		}

		static VkSubresourceLayout doGetDstLayout( VkDevice device
			, VkImage image
			, VkBufferImageCopy const & copyInfo )
		{
			VkSubresourceLayout layout{};
			get( device )->getImageSubresourceLayout( image
				, VkImageSubresource
				{
					copyInfo.imageSubresource.aspectMask,
					copyInfo.imageSubresource.mipLevel,
					copyInfo.imageSubresource.baseArrayLayer,
				}
				, layout );

			if ( copyInfo.imageOffset.z )
			{
				layout.offset += layout.depthPitch * copyInfo.imageOffset.z;
			}

			return layout;
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

		static VkExtent3D getTexelBlockExtent( VkFormat format )
		{
			VkExtent3D result{ 1u, 1u, 1u };

			if ( ashes::isCompressedFormat( format ) )
			{
				auto extent = ashes::getMinimalExtent2D( format );
				result.width = extent.width;
				result.height = extent.height;
			}

			return result;
		}

		static uint32_t getBlockCount( uint32_t value, uint32_t blockExtent )
		{
			return std::max( 1u, ( value + blockExtent - 1u ) / blockExtent );
		}

		static void doCopyMapped( VkFormat format
			, VkBufferImageCopy const & copyInfo
			, D3D11_MAPPED_SUBRESOURCE srcBuffer
			, D3D11_BOX const & srcBox
			, D3D11_MAPPED_SUBRESOURCE dstBuffer
			, VkSubresourceLayout const & )
		{
			auto const blockExtent = getTexelBlockExtent( format );
			auto const blockByteSize = getTexelBlockByteSize( blockExtent, format );
			auto const widthBlocks = getBlockCount( copyInfo.imageExtent.width, blockExtent.width );
			auto const heightBlocks = getBlockCount( copyInfo.imageExtent.height, blockExtent.height );
			auto const depthBlocks = getBlockCount( copyInfo.imageExtent.depth, blockExtent.depth );
			auto const bufferRowBlocks = getBlockCount( getBufferRowPitch( copyInfo ), blockExtent.width );
			auto const bufferHeightBlocks = getBlockCount( getBufferHeightPitch( copyInfo ), blockExtent.height );
			auto const copyRowBytes = VkDeviceSize( widthBlocks ) * blockByteSize;
			auto const bufferRowPitch = VkDeviceSize( bufferRowBlocks ) * blockByteSize;
			auto const bufferDepthPitch = bufferRowPitch * bufferHeightBlocks;
			auto const dstX = VkDeviceSize( uint32_t( copyInfo.imageOffset.x ) / blockExtent.width ) * blockByteSize;
			auto const dstY = uint32_t( copyInfo.imageOffset.y ) / blockExtent.height;
			auto const dstZ = uint32_t( copyInfo.imageOffset.z ) / blockExtent.depth;

			if ( !srcBuffer.pData || !dstBuffer.pData || dstX >= dstBuffer.RowPitch )
			{
				return;
			}

			auto const rowBytes = std::min( copyRowBytes, VkDeviceSize( dstBuffer.RowPitch ) - dstX );
			auto srcData = reinterpret_cast< uint8_t const * >( srcBuffer.pData ) + srcBox.left;
			auto dstData = reinterpret_cast< uint8_t * >( dstBuffer.pData )
				+ dstZ * dstBuffer.DepthPitch
				+ dstY * dstBuffer.RowPitch
				+ dstX;

			for ( auto z = 0u; z < depthBlocks; ++z )
			{
				auto srcSlice = srcData + z * bufferDepthPitch;
				auto dstSlice = dstData + z * dstBuffer.DepthPitch;

				for ( auto y = 0u; y < heightBlocks; ++y )
				{
					std::memcpy( dstSlice + y * dstBuffer.RowPitch
						, srcSlice + y * bufferRowPitch
						, size_t( rowBytes ) );
				}
			}
		}


		// v13.46: direct upload. Texture uploads come from host-visible staging buffers whose
		// bytes live in the bridge's CPU shadow (vkMapMemory returns that shadow). The old path
		// mapped the D3D11 staging buffer for READ (a GPU sync point), mapped a staging image for
		// WRITE, copied row by row and then issued CopySubresourceRegion. For DEFAULT-usage,
		// single-sample, non-depth destinations one UpdateSubresource straight from the shadow does
		// the same with no stall. RW_GPU_UPLOAD_DIRECT=0 restores the old path.
		static bool directUploadEnabled()
		{
			static bool const value = []()
			{
				auto env = std::getenv( "RW_GPU_UPLOAD_DIRECT" );
				return !( env && env[0] == '0' );
			}();
			return value;
		}

		static bool canUpdateDirect( ID3D11Resource * resource )
		{
			if ( !resource ) return false;
			D3D11_RESOURCE_DIMENSION dim{};
			resource->GetType( &dim );
			if ( dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D )
			{
				D3D11_TEXTURE2D_DESC desc{};
				static_cast< ID3D11Texture2D * >( resource )->GetDesc( &desc );
				return desc.Usage == D3D11_USAGE_DEFAULT && desc.SampleDesc.Count == 1u
					&& ( desc.BindFlags & D3D11_BIND_DEPTH_STENCIL ) == 0u;
			}
			if ( dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D )
			{
				D3D11_TEXTURE3D_DESC desc{};
				static_cast< ID3D11Texture3D * >( resource )->GetDesc( &desc );
				return desc.Usage == D3D11_USAGE_DEFAULT;
			}
			if ( dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D )
			{
				D3D11_TEXTURE1D_DESC desc{};
				static_cast< ID3D11Texture1D * >( resource )->GetDesc( &desc );
				return desc.Usage == D3D11_USAGE_DEFAULT;
			}
			return false;
		}

		static bool doDirectUpdate( ID3D11DeviceContext * context
			, VkFormat format
			, VkBufferImageCopy const & copyInfo
			, uint8_t const * shadow
			, VkDeviceSize shadowSize
			, VkDeviceSize srcBase
			, D3D11_BOX const & srcBox
			, ID3D11Resource * dst
			, UINT dstSubresource )
		{
			auto const blockExtent = getTexelBlockExtent( format );
			auto const blockByteSize = getTexelBlockByteSize( blockExtent, format );
			auto const heightBlocks = getBlockCount( copyInfo.imageExtent.height, blockExtent.height );
			auto const bufferRowBlocks = getBlockCount( getBufferRowPitch( copyInfo ), blockExtent.width );
			auto const bufferHeightBlocks = getBlockCount( getBufferHeightPitch( copyInfo ), blockExtent.height );
			auto const rowPitch = VkDeviceSize( bufferRowBlocks ) * blockByteSize;
			auto const depthPitch = rowPitch * bufferHeightBlocks;
			auto const begin = srcBase + srcBox.left;
			auto const needed = rowPitch * ( heightBlocks - 1u ) + VkDeviceSize( getBlockCount( copyInfo.imageExtent.width, blockExtent.width ) ) * blockByteSize;
			if ( !shadow || begin + needed > shadowSize || rowPitch == 0u ) return false;
			D3D11_BOX box
			{
				UINT( copyInfo.imageOffset.x ),
				UINT( copyInfo.imageOffset.y ),
				UINT( copyInfo.imageOffset.z ),
				UINT( copyInfo.imageOffset.x ) + copyInfo.imageExtent.width,
				UINT( copyInfo.imageOffset.y ) + copyInfo.imageExtent.height,
				UINT( copyInfo.imageOffset.z ) + 1u,
			};
			// Block-compressed: D3D11 boxes are in physical (whole-block) units, so the tail mips
			// (2x2, 1x1) use a 4x4 box; the source rows already hold whole blocks.
			if ( blockExtent.width > 1u ) box.right = box.left + getBlockCount( copyInfo.imageExtent.width, blockExtent.width ) * blockExtent.width;
			if ( blockExtent.height > 1u ) box.bottom = box.top + heightBlocks * blockExtent.height;
			static int dbg = 0; if ( envflags::uploadTrace() && dbg++ < 12 ) std::fprintf( stderr, "[RW-GPU-UPLOAD] direct dst=%p sub=%u box=%u,%u,%u-%u,%u,%u pitch=%llu src+%llu first=%02X%02X%02X%02X fmt=%d\n", (void*)dst, dstSubresource, box.left, box.top, box.front, box.right, box.bottom, box.back, (unsigned long long)rowPitch, (unsigned long long)begin, shadow[begin], shadow[begin+1], shadow[begin+2], shadow[begin+3], int(format) );
			context->UpdateSubresource( dst, dstSubresource, &box, shadow + begin, UINT( rowPitch ), UINT( depthPitch ) );
			return true;
		}

		static VkBuffer getStagingBuffer( VkDevice device
			, VkDeviceSize size
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

		static bool isMappable( D3D11_USAGE usage, UINT cpuAccessFlags, UINT requiredAccess )
		{
			return usage == D3D11_USAGE_STAGING
				&& ( cpuAccessFlags & requiredAccess ) == requiredAccess;
		}

		static bool isMappableForRead( VkBuffer buffer )
		{
			D3D11_BUFFER_DESC desc{};
			get( buffer )->getBuffer()->GetDesc( &desc );
			return isMappable( desc.Usage, desc.CPUAccessFlags, D3D11_CPU_ACCESS_READ );
		}

		static bool isMappableForWrite( VkImage image )
		{
			switch ( get( image )->getType() )
			{
			case VK_IMAGE_TYPE_1D:
				{
					D3D11_TEXTURE1D_DESC desc{};
					get( image )->getTexture1D()->GetDesc( &desc );
					return isMappable( desc.Usage, desc.CPUAccessFlags, D3D11_CPU_ACCESS_WRITE );
				}
			case VK_IMAGE_TYPE_2D:
				{
					D3D11_TEXTURE2D_DESC desc{};
					get( image )->getTexture2D()->GetDesc( &desc );
					return isMappable( desc.Usage, desc.CPUAccessFlags, D3D11_CPU_ACCESS_WRITE );
				}
			case VK_IMAGE_TYPE_3D:
				{
					D3D11_TEXTURE3D_DESC desc{};
					get( image )->getTexture3D()->GetDesc( &desc );
					return isMappable( desc.Usage, desc.CPUAccessFlags, D3D11_CPU_ACCESS_WRITE );
				}
			default:
				return false;
			}
		}

		static VkBufferCopy doGetCopyToStaging( VkFormat format
			, ArrayView< VkBufferImageCopy const > const & copyInfos )
		{
			static auto constexpr MaxValue = std::numeric_limits< VkDeviceSize >::max();
			static auto constexpr MinValue = std::numeric_limits< VkDeviceSize >::lowest();
			VkDeviceSize minOffset = MaxValue;
			VkDeviceSize maxSize = MinValue;

			for ( auto & copyInfo : copyInfos )
			{
				auto size = doGetBufferSize( format, copyInfo );
				minOffset = std::min( minOffset, copyInfo.bufferOffset );
				maxSize = std::max( maxSize, copyInfo.bufferOffset + size );
			}

			return VkBufferCopy
			{
				minOffset,
				0u,
				maxSize - minOffset,
			};
		}

		static Optional< CopyToStagingProcess > getCopyToStagingProcess( VkDevice device
			, VkImage dst
			, ArrayView< VkBufferImageCopy const > const & copyInfos
			, bool srcMappable
			, VkBuffer & src
			, VkBufferImageCopy & mapCopyInfo )
		{
			Optional< CopyToStagingProcess > result = ashes::nullopt;

			if ( !srcMappable )
			{
				auto copyToStaging = doGetCopyToStaging( get( dst )->getFormat(), copyInfos );
				CopyToStagingProcess process;
				process.copyToStaging = copyToStaging;
				process.stagingSrc = getStagingBuffer( device
					, process.copyToStaging.size
					, process.stagingSrcMemory );

				mapCopyInfo.bufferOffset = copyToStaging.srcOffset;
				src = process.stagingSrc;
				result = process;
			}
			else
			{
				mapCopyInfo.bufferOffset = 0u;
			}

			return result;
		}

		static void getCopyFromStagingProcess( VkDevice device
			, VkImage image
			, bool dstMappable
			, VkBufferImageCopy bufferImageCopy
			, MapCopyImage & mapCopyImage )
		{
			if ( !dstMappable )
			{
				VkImageSubresourceLayers stagingImageSubresource = bufferImageCopy.imageSubresource;
				stagingImageSubresource.layerCount = 1u;
				stagingImageSubresource.baseArrayLayer = 0u;

				CopyFromStagingProcess process;
				process.stagingDst = get( device )->getStagingImage( image
					, process.stagingDstMemory
					, &process.stagingOwner );

				mapCopyImage.dst = process.stagingDst;
				mapCopyImage.dstMemory = process.stagingDstMemory;
				mapCopyImage.dstSubresource = get( process.stagingDst )->getNativeSubresource(
					stagingImageSubresource.mipLevel
					, stagingImageSubresource.baseArrayLayer );
				process.stagingDstSubresource = mapCopyImage.dstSubresource;

				process.copyFromStaging.srcOffset = bufferImageCopy.imageOffset;
				process.copyFromStaging.srcSubresource = stagingImageSubresource;

				process.copyFromStaging.dstOffset = bufferImageCopy.imageOffset;
				process.copyFromStaging.dstSubresource = bufferImageCopy.imageSubresource;
				assert( process.copyFromStaging.dstSubresource.layerCount == 1u );

				process.copyFromStaging.extent = bufferImageCopy.imageExtent;

				// We copy to layer 0 of staging image (it has only 1 layer).
				bufferImageCopy.imageSubresource.baseArrayLayer = 0u;
				mapCopyImage.dstLayout = doGetDstLayout( device, mapCopyImage.dst, bufferImageCopy );
				mapCopyImage.copyFromStaging = process;
			}
		}

		// v12: decode BC blocks from the mapped source buffer and write RGBA8
		// texels into the decoded native texture, in bounded strips.
		static void doDecodedCopy( VkDevice device
			, ID3D11DeviceContext * context
			, MapCopyImage const & mapCopy
			, D3D11_MAPPED_SUBRESOURCE srcMapped )
		{
			auto const & copyInfo = mapCopy.mapCopy;
			auto image = get( mapCopy.dst );
			auto const format = image->getFormat();
			auto texture = image->getTexture2D();
			if ( !srcMapped.pData || !texture )
			{
				return;
			}
			LARGE_INTEGER start{};
			LARGE_INTEGER end{};
			LARGE_INTEGER frequency{};
			QueryPerformanceCounter( &start );
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc( &desc );
			auto const mip = mapCopy.dstSubresource % desc.MipLevels;
			auto const mipWidth = std::max( 1u, desc.Width >> mip );
			auto const mipHeight = std::max( 1u, desc.Height >> mip );
			auto const x0 = uint32_t( copyInfo.imageOffset.x );
			auto const y0 = uint32_t( copyInfo.imageOffset.y );
			uint32_t const key[] = { uint32_t( format ), image->getDimensions().width, image->getDimensions().height
				, copyInfo.imageSubresource.mipLevel, x0, y0, copyInfo.imageExtent.width, copyInfo.imageExtent.height };
			if ( copyInfo.imageOffset.x < 0 || copyInfo.imageOffset.y < 0
				|| ( x0 & 3u ) || ( y0 & 3u ) || x0 >= mipWidth || y0 >= mipHeight )
			{
				rdiag::report( "bc-decoded-upload-rejected", rdiag::hashBytes( 0x5245u, key, sizeof( key ) )
					, "vkFormat=%u level=%u offset=%d,%d extent=%ux%u mip=%ux%u reason=offset-not-block-aligned-or-outside-mip"
					, unsigned( format ), copyInfo.imageSubresource.mipLevel, copyInfo.imageOffset.x, copyInfo.imageOffset.y
					, copyInfo.imageExtent.width, copyInfo.imageExtent.height, mipWidth, mipHeight );
				return;
			}
			auto const copyWidth = std::min( copyInfo.imageExtent.width, mipWidth - x0 );
			auto const copyHeight = std::min( copyInfo.imageExtent.height, mipHeight - y0 );
			auto const blockBytes = bc::blockBytes( format );
			auto const widthBlocks = getBlockCount( copyWidth, 4u );
			auto const heightBlocks = getBlockCount( copyHeight, 4u );
			auto const bufferRowPitch = VkDeviceSize( getBlockCount( getBufferRowPitch( copyInfo ), 4u ) ) * blockBytes;
			auto const pitch = widthBlocks * 16u;
			uint32_t constexpr StripBytes = 4u * 1024u * 1024u;
			auto const stripBlockRows = std::max( 1u, std::min( heightBlocks, StripBytes / std::max( 1u, pitch * 4u ) ) );
			std::vector< uint8_t > decoded( size_t( pitch ) * stripBlockRows * 4u );
			auto const * src = reinterpret_cast< uint8_t const * >( srcMapped.pData ) + mapCopy.srcBox.left;
			auto d3ddevice = get( device )->getDevice();
			for ( uint32_t row = 0u; row < heightBlocks; row += stripBlockRows )
			{
				auto const count = std::min( stripBlockRows, heightBlocks - row );
				if ( !bcdecode::rows( format, src + row * bufferRowPitch, bufferRowPitch, widthBlocks, count
					, decoded.data(), pitch ) )
				{
					rdiag::report( "bc-decoded-upload-rejected", rdiag::hashBytes( 0x5246u, key, sizeof( key ) )
						, "vkFormat=%u reason=format-not-decodable", unsigned( format ) );
					return;
				}
				D3D11_BOX box{ x0, y0 + row * 4u, 0u
					, x0 + copyWidth, std::min( y0 + ( row + count ) * 4u, y0 + copyHeight ), 1u };
				if ( box.top >= box.bottom )
				{
					break;
				}
				bcdecode::updateBox( d3ddevice, context, texture, mapCopy.dstSubresource, box
					, decoded.data(), pitch, pitch * count * 4u );
			}
			QueryPerformanceCounter( &end );
			QueryPerformanceFrequency( &frequency );
			auto const micros = frequency.QuadPart
				? ( unsigned long long )( ( end.QuadPart - start.QuadPart ) * 1000000ll / frequency.QuadPart )
				: 0ull;
			rdiag::report( "bc-decoded-upload", rdiag::hashBytes( 0x5247u, key, sizeof( key ) )
				, "vkFormat=%u logical=%ux%u level=%u nativeSubresource=%u offset=%u,%u extent=%ux%u blocks=%ux%u decodeMicros=%llu"
				, unsigned( format ), image->getDimensions().width, image->getDimensions().height
				, copyInfo.imageSubresource.mipLevel, mapCopy.dstSubresource, x0, y0, copyWidth, copyHeight
				, widthBlocks, heightBlocks, micros );
		}

		static MapCopyProcess getMapCopyProcess( VkDevice device
			, VkBuffer src
			, VkImage dst
			, ArrayView< VkBufferImageCopy const > const & copyInfos
			, VkBufferImageCopy const & baseBufferImageCopy
			, bool dstMappable )
		{
			MapCopyProcess result
			{
				src,
				get( src )->getMemory(),
			};

			for ( auto & copy : copyInfos )
			{
				VkBufferImageCopy copyInfo = copy;
				// We process images array level per array level
				copyInfo.imageSubresource.layerCount = 1u;
				// We process images depth slice per depth slice
			copyInfo.imageExtent.depth = 1u;
			copyInfo.bufferOffset -= baseBufferImageCopy.bufferOffset;
			copyInfo.bufferOffset += get( src )->getNativeOffset();
			auto slicePitch = getSize( VkExtent3D{ getBufferRowPitch( copyInfo ), getBufferHeightPitch( copyInfo ), 1u }, get( dst )->getFormat() );

				for ( uint32_t layer = 0u; layer < copy.imageSubresource.layerCount; ++layer )
				{
					for ( int32_t slice = copy.imageOffset.z; slice < copy.imageOffset.z + int32_t( copy.imageExtent.depth ); ++slice )
					{
						auto srcBox = doGetSrcBox( get( dst )->getFormat(), copyInfo, uint32_t( slice ) );
						auto dstLayout = doGetDstLayout( device, dst, copyInfo );
						MapCopyImage mapCopyImage
						{
							dst,
							get( dst )->getMemory(),
							copyInfo,
							srcBox,
							get( dst )->getNativeSubresource(
								copyInfo.imageSubresource.mipLevel
								, copyInfo.imageSubresource.baseArrayLayer ),
							dstLayout,
						};
						getCopyFromStagingProcess( device
							, dst
							, dstMappable
							, copyInfo
							, mapCopyImage );
						result.mapCopyImages.push_back( mapCopyImage );
						copyInfo.imageOffset.z++;
						copyInfo.bufferOffset += slicePitch;
					}

					copyInfo.imageSubresource.baseArrayLayer++;
				}
			}

			return result;
		}

		static BufferToImageCopyProcess buildProcess( VkDevice device
			, ArrayView< VkBufferImageCopy const > const & copyInfos
			, VkBuffer src
			, VkImage dst
			, bool srcMappable
			, bool dstMappable )
		{
			VkBufferImageCopy baseBufferImageCopy{};
			auto copyToStaging = getCopyToStagingProcess( device
				, dst
				, copyInfos
				, srcMappable
				, src
				, baseBufferImageCopy );

			// v12: decoded BC destinations are written by CPU decode + UpdateSubresource;
			// a BC-format staging image would carry the wrong representation.
			auto mapCopy = getMapCopyProcess( device
				, src
				, dst
				, copyInfos
				, baseBufferImageCopy
				, dstMappable || get( dst )->isDecodedBc() );
			return BufferToImageCopyProcess{ copyToStaging, mapCopy };
		}
	}

	CopyBufferToImageCommand::CopyBufferToImageCommand( VkDevice device
		, ArrayView< VkBufferImageCopy const > const & copyInfos
		, VkBuffer src
		, VkImage dst )
		: CommandBase{ device }
		, m_src{ src }
		, m_dst{ dst }
			, m_copyInfos{ copyInfos.begin(), copyInfos.end() }
			, m_format{ getSRVFormat( get( m_dst )->getFormat() ) }
			, m_srcMappable{ copybuftoimg::isMappableForRead( m_src ) }
			, m_dstMappable{ copybuftoimg::isMappableForWrite( m_dst ) }
		, m_process{ copybuftoimg::buildProcess ( device, copyInfos, src, dst, m_srcMappable, m_dstMappable ) }
	{
	}

	CopyBufferToImageCommand::~CopyBufferToImageCommand()noexcept
	{
		if ( m_process.copyToStaging )
		{
			deallocate( m_process.copyToStaging->stagingSrcMemory, get( getDevice() )->getAllocationCallbacks() );
			deallocate( m_process.copyToStaging->stagingSrc, get( getDevice() )->getAllocationCallbacks() );
		}
	}

	void CopyBufferToImageCommand::apply( Context & context )const
	{
		if ( m_process.copyToStaging )
		{
			apply( context, *m_process.copyToStaging );
		}

		apply( context, m_process.mapCopy );
		get( m_dst )->bumpContentVersion(); // v13.14: swizzle shadows rebuild on change
	}

	void CopyBufferToImageCommand::apply( Context & context
		, CopyToStagingProcess const & process )const
	{
		CopyBufferCommand command{ getDevice()
			, process.copyToStaging
			, m_src
			, process.stagingSrc };
		command.apply( context );
	}

	void CopyBufferToImageCommand::apply( Context & context
		, CopyFromStagingProcess const & process )const
	{
		CopyImageCommand command{ getDevice()
			, process.copyFromStaging
			, process.stagingDst
			, m_dst };
		command.apply( context );
	}

	void CopyBufferToImageCommand::apply( Context & context
		, MapCopyProcess const & process )const
	{
		// v13.46: direct path (see copybuftoimg::directUploadEnabled).
		if ( copybuftoimg::directUploadEnabled() && !m_process.copyToStaging )
		{
			auto & srcObject = get( process.src )->getObjectMemory();
			auto srcParent = get( srcObject.deviceMemory );
			auto shadow = srcParent ? srcParent->debugShadow() : nullptr;
			bool const hostVisible = srcParent && ( srcParent->getMemoryPropertyFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) != 0u;
			if ( shadow && hostVisible )
			{
				bool allDirect = true;
				// mapCopyImage.dst is a pooled staging image when the real destination is not
				// mappable; the direct path always writes the real image (m_dst).
				auto dstImage = get( m_dst );
				if ( dstImage->isDecodedBc() || !copybuftoimg::canUpdateDirect( dstImage->getResource() ) )
				{
					allDirect = false;
				}
				if ( allDirect )
				{
					bool ok = true;
					for ( auto & mapCopyImage : process.mapCopyImages )
					{
						ok = copybuftoimg::doDirectUpdate( *context.context
							, dstImage->getFormat()
							, mapCopyImage.mapCopy
							, shadow
							, srcParent->debugShadowSize()
							, srcObject.offset
							, mapCopyImage.srcBox
							, dstImage->getResource()
							, dstImage->getNativeSubresource( mapCopyImage.mapCopy.imageSubresource.mipLevel
								, mapCopyImage.mapCopy.imageSubresource.baseArrayLayer ) ) && ok;
					}
					if ( ok )
					{
						++context.directUploads;
						return;
					}
				}
			}
		}

		D3D11_MAPPED_SUBRESOURCE srcMapped{};

		if ( auto & src = get( process.src )->getObjectMemory();
			VK_SUCCESS == src.lock( *context.context
				, 0u
				, srcMapped )
			&& srcMapped.pData )
		{
			for ( auto & mapCopyImage : process.mapCopyImages )
			{
				if ( get( mapCopyImage.dst )->isDecodedBc() )
				{
					copybuftoimg::doDecodedCopy( getDevice()
						, *context.context
						, mapCopyImage
						, srcMapped );
					continue;
				}

				doMapCopy( context
					, mapCopyImage
					, get( mapCopyImage.dst )->getFormat()
					, srcMapped
					, get( mapCopyImage.dst )->getObjectMemory() );

				if ( mapCopyImage.copyFromStaging )
				{
					apply( context, *mapCopyImage.copyFromStaging );
				}
			}

			src.unlock( *context.context, 0u );
		}
	}

	CommandPtr CopyBufferToImageCommand::clone()const
	{
		return std::make_unique< CopyBufferToImageCommand >( getDevice()
			, makeArrayView( m_copyInfos.data(), m_copyInfos.size() )
			, m_src
			, m_dst );
	}

	void CopyBufferToImageCommand::doMapCopy( Context const & context
		, MapCopyImage const & mapCopy
		, VkFormat format
		, D3D11_MAPPED_SUBRESOURCE srcMapped
		, ObjectMemory const & dst )const
	{
		if ( D3D11_MAPPED_SUBRESOURCE dstMapped{};
			VK_SUCCESS == dst.lock( *context.context
				, mapCopy.dstSubresource
				, dstMapped )
			&& dstMapped.pData )
		{
			copybuftoimg::doCopyMapped( format
				, mapCopy.mapCopy
				, srcMapped
				, mapCopy.srcBox
				, dstMapped
				, mapCopy.dstLayout );
			dst.unlock( *context.context, mapCopy.dstSubresource );
		}
	}

	void CopyBufferToImageCommand::doCopyToStaging( Context & context
		, VkDeviceSize srcOffset
		, VkDeviceSize size
		, VkBuffer src
		, VkBuffer staging )const
	{
		CopyBufferCommand command{ getDevice()
			, VkBufferCopy{ srcOffset, 0u, size }
			, src
			, staging };
		command.apply( context );
	}

	void CopyBufferToImageCommand::doCopyFromStaging( Context & context
		, VkBufferImageCopy const & copyInfo
		, VkImage staging
		, VkImage dst )const
	{
		VkImageSubresourceLayers stagingSurbresouce{ copyInfo.imageSubresource };
		stagingSurbresouce.mipLevel = 0u;
		stagingSurbresouce.baseArrayLayer = 0u;
		CopyImageCommand command{ getDevice()
			, VkImageCopy{ stagingSurbresouce
				, VkOffset3D{}
				, copyInfo.imageSubresource
				, copyInfo.imageOffset
				, copyInfo.imageExtent }
			, staging
			, dst };
		command.apply( context );
	}
}
