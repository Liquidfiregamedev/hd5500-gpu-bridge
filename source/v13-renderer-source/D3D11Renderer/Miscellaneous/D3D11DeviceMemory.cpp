/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#include "Miscellaneous/D3D11DeviceMemory.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"

#include "Core/D3D11Device.hpp"
#include "Core/D3D11Instance.hpp"
#include "Core/D3D11PhysicalDevice.hpp"

#include <ashes/common/Exception.hpp>

#include "ashesd3d11_api.hpp"
#include "Miscellaneous/D3D11BcLayout.hpp"
#include "Miscellaneous/D3D11BcDecode.hpp"
#include "Miscellaneous/D3D11RenderDiagnostics.hpp"
#include "Miscellaneous/D3D11ShadowTexture.hpp"
#include "Miscellaneous/D3D11ResourceMap.hpp"
#include "Miscellaneous/D3D11MemoryTrace.hpp"

#include <cstdlib>
#include <cstdio>

#pragma warning( push )
#pragma warning( disable: 5262 )
#include <iostream>
#pragma warning( pop )

namespace ashes::D3D11_NAMESPACE
{
	//*********************************************************************************************

	namespace mem
	{
		static VkMemoryPropertyFlags getMemoryProperties( uint32_t memoryTypeIndex )
		{
			assert( memoryTypeIndex < Instance::getMemoryProperties().memoryTypeCount
				&& "Wrong deduced memory type" );
			return Instance::getMemoryProperties().memoryTypes[memoryTypeIndex].propertyFlags;
		}

		static UINT getBindFlags( VkImageCreateInfo const & createInfo
			, VkMemoryPropertyFlags propertyFlags )
		{
			return ( ( isSampled( createInfo.usage )
					? D3D11_BIND_SHADER_RESOURCE
					: 0u )
				| ( isRenderable( createInfo.usage, createInfo.format, createInfo.mipLevels )
					? ( isHostVisible( propertyFlags )
						? 0u
						: ( isDepthOrStencilFormat( createInfo.format )
							? D3D11_BIND_DEPTH_STENCIL
							: D3D11_BIND_RENDER_TARGET ) )
					: 0u )
				| ( isStorage( createInfo.usage )
					? D3D11_BIND_UNORDERED_ACCESS
					: 0 ) );
		}

		static UINT getMiscFlags( VkImageCreateInfo const & createInfo )
		{
			return 0u
				| ( checkFlag( createInfo.flags, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT )
					? D3D11_RESOURCE_MISC_TEXTURECUBE
					: 0u );
		}
	}

	namespace
	{
			static VkResult getNativeFailureResult( VkDevice device, HRESULT hr )
			{
				if ( get( device )->getStatus() != VK_SUCCESS
					|| hr == DXGI_ERROR_DEVICE_HUNG
					|| hr == DXGI_ERROR_DEVICE_REMOVED
					|| hr == DXGI_ERROR_DEVICE_RESET
					|| hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR )
				{
					return VK_ERROR_DEVICE_LOST;
				}
				return hr == E_OUTOFMEMORY
					? VK_ERROR_OUT_OF_DEVICE_MEMORY
					: VK_ERROR_INITIALIZATION_FAILED;
		}

			static ID3D11Buffer * getBufferResource( ObjectMemory const & memory
				, D3D11_BUFFER_DESC & desc )
			{
				if ( !memory.resource )
			{
				return nullptr;
			}

			D3D11_RESOURCE_DIMENSION dimension{};
			memory.resource->GetType( &dimension );

			if ( dimension != D3D11_RESOURCE_DIMENSION_BUFFER )
			{
				return nullptr;
			}

				auto result = static_cast< ID3D11Buffer * >( memory.resource );
				result->GetDesc( &desc );
				return result;
			}

			static bool getBufferCopyRange( ObjectMemory const & memory
			, D3D11_BUFFER_DESC const & desc
			, VkDeviceSize poffset
			, VkDeviceSize psize
				, VkDeviceSize & copyOffset
					, UINT & copySize )
				{
					// The shadow remains parent-relative; the native buffer starts at 0.
					auto const capacity = memory.allocateInfo.allocationSize;
					if ( memory.offset >= capacity )
					{
						return false;
					}
					auto const boundSize = std::min( VkDeviceSize( desc.ByteWidth ),
						std::min( memory.size ? memory.size : VkDeviceSize( desc.ByteWidth ),
							capacity - memory.offset ) );
					auto const boundEnd = memory.offset + boundSize;
					if ( poffset >= capacity ) return false;
					copyOffset = std::max( memory.offset, poffset );
					auto const copyEnd = std::min( boundEnd,
					poffset + std::min( psize, capacity - poffset ) );
				if ( copyOffset >= copyEnd ) return false;
				copySize = UINT( copyEnd - copyOffset );
				return copySize != 0u;
			}
		}

	//*********************************************************************************************

	ObjectMemory::ObjectMemory( VkDevice device
		, VkDeviceMemory deviceMemory
		, ID3D11Resource * resource
		, VkDeviceSize offset
		, VkMemoryAllocateInfo allocateInfo
		, D3D11_MAP mapFlags )
		: device{ device }
		, deviceMemory{ deviceMemory }
		, resource{ resource }
		, offset{ offset }
			, allocateInfo{ std::move( allocateInfo ) }
			, mapFlags{ mapFlags }
		{
			memtrace::acquire( this->resource );
		}

	ObjectMemory::ObjectMemory( ObjectMemory && rhs )noexcept
		: device{ rhs.device }
			, deviceMemory{ rhs.deviceMemory }
			, resource{ rhs.resource }
			, offset{ rhs.offset }
				, size{ rhs.size }
				, allocateInfo{ std::move( rhs.allocateInfo ) }
				, mapFlags{ rhs.mapFlags }
				, mipPrefix{ rhs.mipPrefix }
				, decodedFrom{ rhs.decodedFrom }
				, nativeTextureBytes{ rhs.nativeTextureBytes }
				, mapStaging{ rhs.mapStaging }
				, subresources{ std::move( rhs.subresources ) }
	{
		rhs.mapStaging = nullptr;
		rhs.device = nullptr;
		rhs.deviceMemory = nullptr;
		rhs.resource = nullptr;
		rhs.offset = 0;
		rhs.mapFlags = D3D11_MAP( 0 );
		rhs.nativeTextureBytes = 0u;
	}

	ObjectMemory & ObjectMemory::operator=( ObjectMemory && rhs )noexcept
	{
		serial = nextSerial();   // v13.74: a different resource = a different identity for the index pool
		++contentVersion;
		device = rhs.device;
			deviceMemory = rhs.deviceMemory;
			resource = rhs.resource;
			offset = rhs.offset;
				size = rhs.size;
				allocateInfo = std::move( rhs.allocateInfo );
				mapFlags = rhs.mapFlags;
				mipPrefix = rhs.mipPrefix;
				decodedFrom = rhs.decodedFrom;
				nativeTextureBytes = rhs.nativeTextureBytes;
				mapStaging = rhs.mapStaging;
				subresources = std::move( rhs.subresources );

		rhs.mapStaging = nullptr;
		rhs.device = nullptr;
		rhs.deviceMemory = nullptr;
		rhs.resource = nullptr;
		rhs.offset = 0;
		rhs.mapFlags = D3D11_MAP( 0 );
		rhs.nativeTextureBytes = 0u;

		return *this;
	}

	ObjectMemory::~ObjectMemory()noexcept
	{
		releaseResource();
	}

	void ObjectMemory::releaseResource()noexcept
	{
		auto const releasedBufferBytes = memtrace::bufferBytes( resource );
		memtrace::release( resource );
		if ( releasedBufferBytes )
		{
			memtrace::log( "BUFFER_RESOURCE_RELEASE", deviceMemory, allocateInfo.allocationSize,
				offset, size, 0u, releasedBufferBytes, S_OK );
		}
		if ( resource && nativeTextureBytes )
		{
			memtrace::releaseTexture( nativeTextureBytes, decodedFrom != VK_FORMAT_UNDEFINED );
		}
		nativeTextureBytes = 0u;
		safeRelease( mapStaging );
		safeRelease( resource );
	}

		VkResult ObjectMemory::lock( ID3D11DeviceContext * context
			, UINT subresource
			, D3D11_MAPPED_SUBRESOURCE & data )const
		{
			data = {};
			++contentVersion;   // v13.74: a mapped resource may be written
			if ( !device || get( device )->getStatus() != VK_SUCCESS )
			{
				return VK_ERROR_DEVICE_LOST;
			}

			if ( HRESULT hr = resmap::lock( *this, context, subresource, data );
				!checkError( device, hr, "LockMemory" ) )
			{
				return getNativeFailureResult( device, hr );
		}

		return VK_SUCCESS;
	}

		void ObjectMemory::unlock( ID3D11DeviceContext * context
			, UINT subresource )const
		{
			++contentVersion;   // v13.74: index pool (staging copy-back)
			if ( !device || get( device )->getStatus() != VK_SUCCESS )
			{
				safeRelease( mapStaging );
				return;
			}

			resmap::unlock( *this, context, subresource );
		}

		void ObjectMemory::upload( DeviceContextLock const & context
			, uint8_t const * data
			, UINT subresource
			, VkDeviceSize poffset
			, VkDeviceSize psize )const
		{
			if ( !device || get( device )->getStatus() != VK_SUCCESS || !resource ) return;
				assert( subresource < subresources.size() );
			++contentVersion;   // v13.74: index pool
			D3D11_BUFFER_DESC bufferDesc{};

			if ( auto buffer = getBufferResource( *this, bufferDesc ) )
			{
				VkDeviceSize copyOffset{};
				UINT copySize{};

					if ( getBufferCopyRange( *this, bufferDesc, poffset, psize, copyOffset, copySize ) )
					{
						if ( bufferDesc.Usage == D3D11_USAGE_DEFAULT )
						{
							auto const nativeOffset = copyOffset - offset;
							D3D11_BOX box{ UINT( nativeOffset ), 0u, 0u,
								UINT( nativeOffset + copySize ), 1u, 1u };
							// v13.48: tell the driver the range is not in use by queued GPU work
							// (D3D11.1 D3D11_COPY_NO_OVERWRITE). Plain UpdateSubresource on a buffer the
							// GPU still references makes some drivers copy ("rename") the whole buffer on
							// the CPU first; Waylith's 16 MB storage ring made that very expensive. The
							// Vulkan application already guarantees the range is idle (same rule as the
							// NO_OVERWRITE map path below). RW_GPU_UPDATE_NOOVERWRITE=0 = old call.
							static bool const useNoOverwrite = []()
								{
									char text[4]{};
									auto length = GetEnvironmentVariableA( "RW_GPU_UPDATE_NOOVERWRITE", text, DWORD( sizeof( text ) ) );
									return !( length == 1u && text[0] == '0' );
								}();
							// v13.69: the ID3D11DeviceContext1 interface of a context never changes; ask
							// for it once per context instead of a QueryInterface/Release per upload.
							// (Same object, so no extra reference is kept.)
							thread_local ID3D11DeviceContext * qiContext{};
							thread_local ID3D11DeviceContext1 * qiContext1{};
							ID3D11DeviceContext1 * context1{};
							if ( useNoOverwrite )
							{
								if ( qiContext != *context )
								{
									qiContext1 = nullptr;
									if ( SUCCEEDED( (*context)->QueryInterface( __uuidof( ID3D11DeviceContext1 ), reinterpret_cast< void ** >( &qiContext1 ) ) ) && qiContext1 )
										qiContext1->Release();   // keep the raw pointer; *context holds the object alive
									qiContext = *context;
								}
								context1 = qiContext1;
							}
							if ( context1 )
							{
								context1->UpdateSubresource1( buffer, 0u, &box, data + copyOffset, 0u, 0u, D3D11_COPY_NO_OVERWRITE );
							}
							else
							{
								(*context)->UpdateSubresource( buffer, 0u, &box,
									data + copyOffset, 0u, 0u );
							}
						}
						else
						{
							// Native bytes are local; source shadow bytes remain parent-relative.
							auto access = bufferDesc.Usage == D3D11_USAGE_DYNAMIC
								? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_READ_WRITE;
							if ( access == D3D11_MAP_WRITE_DISCARD )
							{
								// v13.37: write only the requested range with NO_OVERWRITE where the
								// runtime allows it (vertex/index buffers always; constant buffers and
								// buffer SRVs on the D3D11.1 runtime). DISCARD invalidated the whole
								// buffer, so Waylith's 4 MB uniform and 2 MB material buffers were
								// re-uploaded in full for every 256-byte window (~8 MB, >100 ms/frame).
								// The Vulkan application already guarantees that a range it rewrites
								// is not read by in-flight GPU work, which is exactly NO_OVERWRITE's rule.
								bool noOverwrite = false;
								auto const bind = bufferDesc.BindFlags;
								auto dev = get( device );
								if ( bind & D3D11_BIND_CONSTANT_BUFFER ) noOverwrite = dev->canMapNoOverwriteConstantBuffer();
								else if ( bind & ( D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS ) ) noOverwrite = dev->canMapNoOverwriteBufferSrv();
								else if ( bind & ( D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER ) ) noOverwrite = true;
								if ( noOverwrite )
								{
									access = D3D11_MAP_WRITE_NO_OVERWRITE;
								}
								else
								{
									// Restore only this compact buffer after DISCARD, not its pool.
									copyOffset = offset;
									copySize = UINT( std::min( VkDeviceSize( bufferDesc.ByteWidth ),
										allocateInfo.allocationSize - offset ) );
								}
							}
							D3D11_MAPPED_SUBRESOURCE mapped{};
							auto hr = (*context)->Map( buffer, 0u, access, 0u, &mapped );
							if ( SUCCEEDED( hr ) && mapped.pData )
							{
								std::memcpy( static_cast< uint8_t * >( mapped.pData ) + ( copyOffset - offset ),
									data + copyOffset, copySize );
								(*context)->Unmap( buffer, 0u );
							}
						else
						{
							checkError( device, hr, "MapBufferUpload" );
						}
					}
				}

				return;
		}

		if ( shadowtex::upload( *this, context, data, subresource, poffset, psize ) )
		{
			return;
		}

		auto & subresourceLayout = subresources[subresource];
		auto maxOffset = std::max( offset, poffset );
		auto objectOffset = maxOffset - offset;
		auto copySize = VkDeviceSize( psize - std::abs( int64_t( poffset ) - int64_t( offset ) ) );

		if ( psize == WholeSize )
		{
			assert( poffset == 0ull );
			copySize = allocateInfo.allocationSize;
		}
		else if ( copySize > allocateInfo.allocationSize )
		{
			copySize = allocateInfo.allocationSize - objectOffset;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};

		if ( lock( *context, subresource, mapped ) == VK_SUCCESS )
		{
			assert( mapped.pData != nullptr );
			auto dstData = static_cast< uint8_t * >( mapped.pData ) + objectOffset;
			assert( dstData != nullptr );

			if ( mapped.DepthPitch == subresourceLayout.SysMemSlicePitch
				|| mapped.RowPitch == subresourceLayout.SysMemPitch )
			{
				copySize = std::min( copySize, VkDeviceSize( mapped.DepthPitch ) );
				std::memcpy( dstData, data + maxOffset, copySize );
			}
			else
			{
				auto mappedSteps = mapped.DepthPitch / mapped.RowPitch;
				auto systemSteps = subresourceLayout.SysMemSlicePitch / subresourceLayout.SysMemPitch;
				auto rowSize = std::min( mapped.RowPitch, subresourceLayout.SysMemPitch );
				assert( mappedSteps == systemSteps );
				auto src = data + maxOffset;
				auto dst = dstData;

				for ( UINT step = 0u; step < std::min( mappedSteps, systemSteps ); ++step )
				{
					std::memcpy( dst, src, rowSize );
					src += subresourceLayout.SysMemPitch;
					dst += mapped.RowPitch;
				}
			}

			unlock( *context, subresource );
		}
	}

		void ObjectMemory::download( DeviceContextLock const & context
			, uint8_t * data
			, UINT subresource
			, VkDeviceSize poffset
			, VkDeviceSize psize )const
		{
			if ( !device || get( device )->getStatus() != VK_SUCCESS || !resource ) return;
				assert( subresource < subresources.size() );
			D3D11_BUFFER_DESC bufferDesc{};

			if ( auto buffer = getBufferResource( *this, bufferDesc ) )
			{
				VkDeviceSize copyOffset{};
				UINT copySize{};

			if ( !getBufferCopyRange( *this, bufferDesc, poffset, psize, copyOffset, copySize ) )
			{
				return;
			}

			D3D11_BUFFER_DESC stagingDesc = bufferDesc;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.BindFlags = 0u;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			stagingDesc.MiscFlags = 0u;
			stagingDesc.StructureByteStride = 0u;
			ID3D11Buffer * staging{};
			auto device = get( this->device )->getDevice();
			auto hr = device->CreateBuffer( &stagingDesc, nullptr, &staging );

			if ( FAILED( hr ) || !staging )
			{
				checkError( this->device, hr, "CreateStagingBuffer" );
				safeRelease( staging );
				return;
			}

			auto d3dContext = *context;
			d3dContext->CopyResource( staging, buffer );
			D3D11_MAPPED_SUBRESOURCE mapped{};
			hr = d3dContext->Map( staging, 0u, D3D11_MAP_READ, 0u, &mapped );

			if ( SUCCEEDED( hr ) && mapped.pData )
			{
				std::memcpy( data + copyOffset
					, static_cast< uint8_t const * >( mapped.pData ) + ( copyOffset - offset )
					, copySize );
				d3dContext->Unmap( staging, 0u );
			}
			else
			{
				checkError( this->device, hr, "MapStagingBuffer" );
			}

			safeRelease( staging );
			return;
		}

		if ( shadowtex::download( *this, context, data, subresource, poffset, psize ) )
		{
			return;
		}

		auto & subresourceLayout = subresources[subresource];
		auto maxOffset = std::max( this->offset, poffset );
		auto objectOffset = maxOffset - this->offset;
		auto copySize = VkDeviceSize( psize - std::abs( int64_t( poffset ) - int64_t( this->offset ) ) );

		if ( psize == WholeSize )
		{
			assert( poffset == 0ull );
			objectOffset = 0u;
			copySize = allocateInfo.allocationSize;
		}
		else if ( copySize > allocateInfo.allocationSize )
		{
			copySize = allocateInfo.allocationSize - objectOffset;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};

		if ( lock( *context, subresource, mapped ) == VK_SUCCESS )
		{
			assert( mapped.pData != nullptr );
			auto srcData = static_cast< uint8_t const * >( mapped.pData ) + objectOffset;
			assert( srcData != nullptr );

			if ( mapped.RowPitch != subresourceLayout.SysMemPitch )
			{
				auto mappedSteps = mapped.DepthPitch / mapped.RowPitch;
				auto systemSteps = subresourceLayout.SysMemSlicePitch / subresourceLayout.SysMemPitch;
				auto rowSize = std::min( mapped.RowPitch, subresourceLayout.SysMemPitch );
				assert( mappedSteps == systemSteps );
				auto src = srcData;
				auto dst = data + maxOffset;

				for ( UINT step = 0u; step < std::min( mappedSteps, systemSteps ); ++step )
				{
					std::memcpy( dst, src, rowSize );
					src += mapped.RowPitch;
					dst += subresourceLayout.SysMemPitch;
				}
			}
			else
			{
				copySize = std::min( copySize, VkDeviceSize( mapped.DepthPitch ) );
				std::memcpy( data + maxOffset, srcData, copySize );
			}

			unlock( *context, subresource );
		}
	}

	//*********************************************************************************************

	class BufferDeviceMemory
	{
	public:
		BufferDeviceMemory( VkDevice device
				, VkDeviceMemory parent
				, VkDeviceSize offset
				, VkMemoryAllocateInfo allocateInfo
				, VkDeviceSize nativeSize
				, VkBufferUsageFlags targets )
				: m_bufferTargets{ targets }
				, m_propertyFlags{ mem::getMemoryProperties( allocateInfo.memoryTypeIndex ) }
				, m_buffer{ doCreateBuffer( device, nativeSize ) }
				, memory{ device, parent, m_buffer, offset, std::move( allocateInfo ), getBufferMapFlags( m_propertyFlags, m_bufferTargets ) }
			{
				memory.subresources.push_back( { nullptr, UINT( nativeSize ), UINT( nativeSize ) } );
			}

			inline ID3D11Buffer * getBuffer()const
		{
			return m_buffer;
		}

		private:
				ID3D11Buffer * doCreateBuffer( VkDevice device
					, VkDeviceSize nativeSize )const
				{
					ID3D11Buffer * result{ nullptr };
					get( device )->requireHealthy( "CreateBuffer" );
					auto d3ddevice = get( device )->getDevice();

					if ( nativeSize == 0u
						|| nativeSize > std::numeric_limits< UINT >::max() )
					{
						throw Exception{ VK_ERROR_OUT_OF_DEVICE_MEMORY, "Invalid D3D11 buffer allocation size" };
					}

					auto const isStorageBuffer = checkFlag( m_bufferTargets, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT );
					D3D11_BUFFER_DESC desc{};
					desc.ByteWidth = UINT( nativeSize );
					desc.Usage = isStorageBuffer
						? D3D11_USAGE_DEFAULT
						: getBufferUsage( m_propertyFlags, m_bufferTargets );
				desc.BindFlags = getBindFlags( m_bufferTargets );
				desc.CPUAccessFlags = isStorageBuffer
					? 0u
					: getCpuBufferAccessFlags( m_propertyFlags, m_bufferTargets );
				desc.MiscFlags = ( checkFlag( m_bufferTargets, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT )
						? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
						: 0u )
					| ( isStorageBuffer
						? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
						: 0u );
					desc.StructureByteStride = 0;

					if ( !isStorageBuffer && isHostVisible( m_propertyFlags )
						&& desc.BindFlags == 0u )
					{
						// Transfer staging buffers are host-visible Vulkan allocations,
						// which permit both host reads and writes independent of usage.
						desc.Usage = D3D11_USAGE_STAGING;
						desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
					}

					auto hr = d3ddevice->CreateBuffer( &desc, nullptr, &result );

					if ( FAILED( hr ) || !result )
				{
					checkError( device, hr, "CreateBuffer" );
					safeRelease( result );
					throw Exception{ getNativeFailureResult( device, FAILED( hr ) ? hr : E_FAIL ), "CreateBuffer failed" };
				}

				dxDebugName( result, Buffer );

				return result;
			}

	private:
		VkBufferUsageFlags m_bufferTargets;
		VkMemoryPropertyFlags m_propertyFlags;
		ID3D11Buffer * m_buffer{ nullptr };

	public:
		ObjectMemory memory;
	};
	
	//*********************************************************************************************

	class Texture1DDeviceMemory
	{
	public:
		Texture1DDeviceMemory( VkDevice device
			, VkDeviceMemory parent
			, VkDeviceSize offset
			, VkMemoryAllocateInfo allocateInfo
			, VkImageCreateInfo const & createInfo )
			: m_usage{ createInfo.usage }
			, m_propertyFlags{ mem::getMemoryProperties( allocateInfo.memoryTypeIndex ) }
			, m_texture{ doCreateTexture( device, createInfo ) }
			, memory{ device, parent, m_texture, offset, std::move( allocateInfo ), getImageMapFlags( m_propertyFlags, m_usage ) }
		{
			auto extent = getTexelBlockExtent( createInfo.format );
			auto byteSize = getTexelBlockByteSize( extent, createInfo.format );
			memory.subresources.resize( size_t( createInfo.arrayLayers ) * createInfo.mipLevels );

			for ( uint32_t layer = 0u; layer < createInfo.arrayLayers; ++layer )
			{
				for ( uint32_t level = 0u; level < createInfo.mipLevels; ++level )
				{
					auto mipWidth = getSubresourceValue( createInfo.extent.width, level );
					auto mipHeight = getSubresourceValue( createInfo.extent.height, level );
					auto rowPitch = byteSize * mipWidth / ( extent.width * extent.height * extent.depth );
					auto depthPitch = rowPitch * mipHeight * extent.height / ( extent.width * extent.depth );
					memory.subresources[D3D11CalcSubresource( level, layer, createInfo.mipLevels )] = { nullptr, rowPitch, depthPitch };
				}
			}
		}

		inline ID3D11Texture1D * getImage()const
		{
			return m_texture;
		}

	private:
			ID3D11Texture1D * doCreateTexture( VkDevice device
				, VkImageCreateInfo const & createInfo )const
			{
				ID3D11Texture1D * result{};
				get( device )->requireHealthy( "CreateTexture1D" );
				getImageMapFlags( m_propertyFlags, m_usage );
			auto d3ddevice = get( device )->getDevice();
			D3D11_TEXTURE1D_DESC desc{};
			doInitTexDesc( device, createInfo, desc );

				auto const hr = d3ddevice->CreateTexture1D( &desc, nullptr, &result );
				if ( FAILED( hr ) || !result )
				{
					auto const failure = FAILED( hr ) ? hr : E_FAIL;
					checkError( device, failure, "CreateTexture1D" );
					safeRelease( result );
					throw Exception{ getNativeFailureResult( device, failure ), "CreateTexture1D failed" };
				}
				dxDebugName( result, Texture1D );

			return result;
		}

		void doInitTexDesc( VkDevice device
			, VkImageCreateInfo const & createInfo
			, D3D11_TEXTURE1D_DESC & desc )const
		{
			desc.Width = createInfo.extent.width;
			desc.Format = getDxgiFormatGroup( getTextureFormat( createInfo.format ) );
			desc.ArraySize = createInfo.arrayLayers;
			desc.Usage = getImageUsage( m_propertyFlags, m_usage );
			desc.CPUAccessFlags = getCpuImageAccessFlags( m_propertyFlags, m_usage );
			desc.MipLevels = createInfo.mipLevels;
			desc.BindFlags = mem::getBindFlags( createInfo, m_propertyFlags );
			desc.MiscFlags = mem::getMiscFlags( createInfo );

			if ( get( device )->getFeatureLevel() < D3D_FEATURE_LEVEL_11_0 )
			{
				desc.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
			}
		}

	private:
		VkImageUsageFlags m_usage;
		VkMemoryPropertyFlags m_propertyFlags;
		ID3D11Texture1D * m_texture{ nullptr };

	public:
		ObjectMemory memory;
	};

	//*********************************************************************************************

	class Texture2DDeviceMemory
	{
	public:
			Texture2DDeviceMemory( VkDevice device
				, VkDeviceMemory parent
				, VkDeviceSize offset
				, VkMemoryAllocateInfo allocateInfo
				, VkImageCreateInfo const & createInfo )
				: m_usage{ createInfo.usage }
				, m_propertyFlags{ mem::getMemoryProperties( allocateInfo.memoryTypeIndex ) }
				, m_texture{ doCreateTexture( device, createInfo ) }
				, memory{ device, parent, m_texture, offset, std::move( allocateInfo ), getImageMapFlags( m_propertyFlags, m_usage ) }
			{
				auto extent = getTexelBlockExtent( createInfo.format );
				auto byteSize = getTexelBlockByteSize( extent, createInfo.format );
				memory.mipPrefix = bc::prefix( createInfo );
				if ( bc::isDecoded( createInfo ) )
				{
					memory.decodedFrom = createInfo.format;
				}
				memory.nativeTextureBytes = doEstimateNativeBytes( createInfo );
				memtrace::acquireTexture( memory.nativeTextureBytes, memory.decodedFrom != VK_FORMAT_UNDEFINED );
				memory.subresources.resize( size_t( createInfo.arrayLayers ) * createInfo.mipLevels );

				for ( uint32_t layer = 0u; layer < createInfo.arrayLayers; ++layer )
			{
				for ( uint32_t level = 0u; level < createInfo.mipLevels; ++level )
				{
					auto mipWidth = getSubresourceValue( createInfo.extent.width, level );
						auto mipHeight = getSubresourceValue( createInfo.extent.height, level );
						auto rowPitch = byteSize * mipWidth / ( extent.width * extent.height * extent.depth );
						auto depthPitch = rowPitch * mipHeight * extent.height / ( extent.width * extent.depth );
						if ( bc::isFormat( createInfo.format ) )
						{
							rowPitch = bc::rowBytes( createInfo.format, mipWidth );
							depthPitch = rowPitch * bc::rows( mipHeight );
						}
						memory.subresources[D3D11CalcSubresource( level, layer, createInfo.mipLevels )] = { nullptr, rowPitch, depthPitch };
					}
				}
			}

			inline ID3D11Texture2D * getImage()const
			{
			return m_texture;
		}

		private:
			uint64_t doEstimateNativeBytes( VkImageCreateInfo const & createInfo )const
			{
				if ( !m_texture ) return 0u;
				D3D11_TEXTURE2D_DESC desc{};
				m_texture->GetDesc( &desc );
				auto const nativeFormat = bc::nativeFormat( createInfo );
				uint64_t total{};
				for ( UINT mip = 0u; mip < desc.MipLevels; ++mip )
				{
					auto const w = std::max( 1u, desc.Width >> mip );
					auto const h = std::max( 1u, desc.Height >> mip );
					total += bc::isFormat( nativeFormat )
						? uint64_t( bc::rowBytes( nativeFormat, w ) ) * bc::rows( h )
						: uint64_t( ashes::getSize( VkExtent3D{ w, h, 1u }, nativeFormat ) );
				}
				return total * std::max( 1u, desc.ArraySize ) * std::max( 1u, desc.SampleDesc.Count );
			}

			ID3D11Texture2D * doCreateTexture( VkDevice device
			, VkImageCreateInfo const & createInfo )const
			{
				ID3D11Texture2D * result{};
				get( device )->requireHealthy( "CreateTexture2D" );
				auto d3ddevice = get( device )->getDevice();
			D3D11_TEXTURE2D_DESC desc{};
			doInitTexDesc( device, createInfo, desc );

			static volatile LONG textureCallCount{};
			auto const callId = InterlockedIncrement( &textureCallCount );
			auto const trace = envflags::textureTrace() && callId <= 512;   // v13.62: cached
			auto logDescriptor = [&]( char const * stage, HRESULT hr )
			{
				std::fprintf( stderr,
					"[RW-GPU-TEX2D] %s id=%ld hr=0x%08lX vkFormat=%u extent=%ux%ux%u mips=%u layers=%u samples=%u tiling=%u vkUsage=0x%X vkFlags=0x%X memoryProps=0x%X dxgiFormat=%u d3dUsage=%u bind=0x%X cpu=0x%X misc=0x%X quality=%u\n",
					stage, long( callId ), static_cast< unsigned long >( hr ),
					unsigned( createInfo.format ), createInfo.extent.width, createInfo.extent.height, createInfo.extent.depth,
					createInfo.mipLevels, createInfo.arrayLayers, unsigned( createInfo.samples ), unsigned( createInfo.tiling ),
					unsigned( createInfo.usage ), unsigned( createInfo.flags ), unsigned( m_propertyFlags ),
					unsigned( desc.Format ), unsigned( desc.Usage ), desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags, desc.SampleDesc.Quality );
				std::fflush( stderr );
			};
			if ( trace ) { logDescriptor( "BEGIN", S_OK ); }
			auto hr = d3ddevice->CreateTexture2D( &desc, nullptr, &result );
			if ( trace || FAILED( hr ) || !result ) { logDescriptor( "END", FAILED( hr ) ? hr : ( result ? S_OK : E_INVALIDARG ) ); }
			if ( FAILED( hr ) || !result )
			{
				checkError( device, FAILED( hr ) ? hr : E_INVALIDARG, "CreateTexture2D" );
				safeRelease( result );
				throw Exception{ getNativeFailureResult( device, FAILED( hr ) ? hr : E_FAIL ), "CreateTexture2D failed" };
			}
			dxDebugName( result, Texture2D );

			return result;
		}

			void doInitTexDesc( VkDevice device
				, VkImageCreateInfo const & createInfo
				, D3D11_TEXTURE2D_DESC & desc )const
			{
				auto d3ddevice = get( device )->getDevice();
				auto const representation = bc::representation( createInfo );
				auto const prefix = bc::prefix( createInfo );
				auto const width = uint64_t( createInfo.extent.width ) << prefix;
				auto const height = uint64_t( createInfo.extent.height ) << prefix;
				if ( representation == bc::Representation::DecodedRgba8
					|| representation == bc::Representation::Unsupported )
				{
					auto const rawPrefix = bc::rawPrefix( createInfo );
					uint32_t const key[] = { uint32_t( createInfo.format ), createInfo.extent.width, createInfo.extent.height
						, createInfo.mipLevels, createInfo.arrayLayers, uint32_t( createInfo.usage ), uint32_t( createInfo.flags )
						, uint32_t( representation ) };
					rdiag::report( representation == bc::Representation::DecodedRgba8 ? "bc-representation-decoded" : "bc-representation-unsupported"
						, rdiag::hashBytes( 0x5243u, key, sizeof( key ) )
						, "vkFormat=%u logical=%ux%u mips=%u layers=%u usage=0x%X flags=0x%X tiling=%u samples=%u representation=%s prefixedNative=%llux%llu limit=%u decodable=%u fallbackEnabled=%u logicalBcBytes=%llu decodedRgba8Bytes=%llu"
						, unsigned( createInfo.format ), createInfo.extent.width, createInfo.extent.height
						, createInfo.mipLevels, createInfo.arrayLayers, unsigned( createInfo.usage ), unsigned( createInfo.flags )
						, unsigned( createInfo.tiling ), unsigned( createInfo.samples ), bc::representationName( representation )
						, static_cast< unsigned long long >( uint64_t( createInfo.extent.width ) << rawPrefix )
						, static_cast< unsigned long long >( uint64_t( createInfo.extent.height ) << rawPrefix )
						, unsigned( D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ), unsigned( bc::isDecodable( createInfo.format ) )
						, unsigned( bc::decodeFallbackEnabled() )
						, static_cast< unsigned long long >( bc::chainBytes( createInfo, false ) )
						, static_cast< unsigned long long >( bc::chainBytes( createInfo, true ) ) );
				}
				if ( representation == bc::Representation::Unsupported
					|| width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION )
				{
					throw Exception{ VK_ERROR_FORMAT_NOT_SUPPORTED, "BC native mip backing exceeds D3D11 texture dimensions and no exact decoded representation applies" };
				}
				desc.Width = UINT( width );
				desc.Height = UINT( height );
				desc.Format = getDxgiFormatGroup( getTextureFormat( bc::nativeFormat( createInfo ) ) );
				desc.ArraySize = createInfo.arrayLayers;
				desc.Usage = getImageUsage( m_propertyFlags, m_usage );
				desc.CPUAccessFlags = getCpuImageAccessFlags( m_propertyFlags, m_usage );
				desc.MipLevels = createInfo.mipLevels + prefix;
				desc.SampleDesc.Count = UINT( createInfo.samples );
				desc.SampleDesc.Quality = 0;
				desc.BindFlags = mem::getBindFlags( createInfo, m_propertyFlags );
			desc.MiscFlags = mem::getMiscFlags( createInfo );

			if ( isHostVisible( m_propertyFlags ) )
			{
				// Host-visible Vulkan memory has a CPU shadow. Pipeline resources
				// must remain real GPU textures, not STAGING textures with bind flags.
				desc.BindFlags = mem::getBindFlags( createInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
				if ( desc.BindFlags != 0u )
				{
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.CPUAccessFlags = 0u;
				}
			}

			if ( desc.SampleDesc.Count > 1 )
			{
				HRESULT hr = d3ddevice->CheckMultisampleQualityLevels( desc.Format
					, desc.SampleDesc.Count
					, &desc.SampleDesc.Quality );

				if ( hr == S_OK && desc.SampleDesc.Quality > 0 )
				{
					desc.SampleDesc.Quality--;
				}
			}

			if ( get( device )->getFeatureLevel() < D3D_FEATURE_LEVEL_11_0 )
			{
				desc.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
			}
		}

	private:
		VkImageUsageFlags m_usage;
		VkMemoryPropertyFlags m_propertyFlags;
		ID3D11Texture2D * m_texture{ nullptr };

	public:
		ObjectMemory memory;
	};
	
	//*********************************************************************************************

	class Texture3DDeviceMemory
	{
	public:
		Texture3DDeviceMemory( VkDevice device
			, VkDeviceMemory parent
			, VkDeviceSize offset
			, VkMemoryAllocateInfo allocateInfo
			, VkImageCreateInfo const & createInfo )
			: m_usage{ createInfo.usage }
			, m_propertyFlags{ mem::getMemoryProperties( allocateInfo.memoryTypeIndex ) }
			, m_texture{ doCreateTexture( device, createInfo ) }
			, memory{ device, parent, m_texture, offset, std::move( allocateInfo ), getImageMapFlags( m_propertyFlags, m_usage ) }
		{
			auto extent = getTexelBlockExtent( createInfo.format );
			auto byteSize = getTexelBlockByteSize( extent, createInfo.format );
			memory.subresources.resize( size_t( createInfo.arrayLayers ) * createInfo.mipLevels );

			for ( uint32_t layer = 0u; layer < createInfo.arrayLayers; ++layer )
			{
				for ( uint32_t level = 0u; level < createInfo.mipLevels; ++level )
				{
					auto mipWidth = getSubresourceValue( createInfo.extent.width, level );
					auto mipHeight = getSubresourceValue( createInfo.extent.height, level );
					auto rowPitch = byteSize * mipWidth / ( extent.width * extent.height * extent.depth );
					auto depthPitch = rowPitch * mipHeight * extent.height / ( extent.width * extent.depth );
					memory.subresources[D3D11CalcSubresource( level, layer, createInfo.mipLevels )] = { nullptr, rowPitch, depthPitch };
				}
			}
		}

		inline ID3D11Texture3D * getImage()const
		{
			return m_texture;
		}

	private:
			ID3D11Texture3D * doCreateTexture( VkDevice device
			, VkImageCreateInfo const & createInfo )const
			{
				ID3D11Texture3D * result{};
				get( device )->requireHealthy( "CreateTexture3D" );
				auto d3ddevice = get( device )->getDevice();
			D3D11_TEXTURE3D_DESC desc{};
			doInitTexDesc( device, createInfo, desc );

				auto const hr = d3ddevice->CreateTexture3D( &desc, nullptr, &result );
				if ( FAILED( hr ) || !result )
				{
					auto const failure = FAILED( hr ) ? hr : E_FAIL;
					checkError( device, failure, "CreateTexture3D" );
					safeRelease( result );
					throw Exception{ getNativeFailureResult( device, failure ), "CreateTexture3D failed" };
				}
				dxDebugName( result, Texture3D );

			return result;
		}

		void doInitTexDesc( VkDevice device
			, VkImageCreateInfo const & createInfo
			, D3D11_TEXTURE3D_DESC & desc )const
		{
			desc.Width = createInfo.extent.width;
			desc.Height = createInfo.extent.height;
			desc.Depth = createInfo.extent.depth;
			desc.Format = getDxgiFormatGroup( getTextureFormat( createInfo.format ) );
			desc.Usage = getImageUsage( m_propertyFlags, m_usage );
			desc.CPUAccessFlags = getCpuImageAccessFlags( m_propertyFlags, m_usage );
			desc.MipLevels = createInfo.mipLevels;
			desc.BindFlags = mem::getBindFlags( createInfo, m_propertyFlags );
			desc.MiscFlags = mem::getMiscFlags( createInfo );

			if ( get( device )->getFeatureLevel() < D3D_FEATURE_LEVEL_11_0 )
			{
				desc.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
			}
		}

	private:
		VkImageUsageFlags m_usage;
		VkMemoryPropertyFlags m_propertyFlags;
		ID3D11Texture3D * m_texture{ nullptr };

	public:
		ObjectMemory memory;
	};

	//*********************************************************************************************

		DeviceMemory::DeviceMemory( VkDevice device
		, VkMemoryAllocateInfo allocateInfo )
		: m_device{ device }
		, m_allocateInfo{ std::move( allocateInfo ) }
			, m_propertyFlags{ mem::getMemoryProperties( m_allocateInfo.memoryTypeIndex ) }
		{
			get( m_device )->requireHealthy( "Allocate device memory" );
			thread_local uint8_t defaultInitValue = 10u;

		if ( ashes::checkFlag( m_propertyFlags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) )
		{
			m_data.resize( allocateInfo.allocationSize, defaultInitValue );
			++defaultInitValue;
		}
	}

	DeviceMemory::~DeviceMemory()noexcept
	{
		m_lifetime.reset();
		memtrace::log( "PARENT_FREE_BEGIN", this, m_allocateInfo.allocationSize,
			0u, 0u, m_objects.size(), 0u, S_OK );
		if ( !m_objects.empty() )
		{
			onDestroy( get( this ) );
		}
	}

	VkResult DeviceMemory::bindToBuffer( VkBuffer buffer
		, VkDeviceSize memoryOffset
		, ObjectMemory *& objectMemory )
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;

		auto const requirements = get( buffer )->getMemoryRequirements();
		if ( memoryOffset > m_allocateInfo.allocationSize
			|| requirements.size > m_allocateInfo.allocationSize - memoryOffset )
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}

		try
			{
				memtrace::log( "BUFFER_BIND_BEGIN", this, m_allocateInfo.allocationSize,
					memoryOffset, get( buffer )->getSize(), m_objects.size(),
					UINT( std::min( requirements.size, VkDeviceSize( std::numeric_limits< UINT >::max() ) ) ), S_OK );
				BufferDeviceMemory impl
				{
					m_device,
					get( this ),
					memoryOffset,
					m_allocateInfo,
					requirements.size,
					get( buffer )->getUsage()
				};
				impl.memory.size = requirements.size;
			m_objects.emplace_back( std::make_unique< ObjectMemory >( std::move( impl.memory ) ) );
			objectMemory = m_objects.back().get();
			memtrace::log( "BUFFER_BIND_END", this, m_allocateInfo.allocationSize,
				memoryOffset, get( buffer )->getSize(), m_objects.size(),
				memtrace::bufferBytes( objectMemory->resource ), S_OK );
			// Never-mapped allocation contents are undefined; do not reacquire
			// the queue's context lock just to upload an unused CPU shadow.
			if ( m_mappedSize != 0u )
			{
				auto context{ get( m_device )->getImmediateContext() };
				updateUpload( context, *objectMemory, 0ULL, WholeSize, 0u );
			}
			result = VK_SUCCESS;
		}
		catch ( Exception & exc )
		{
			std::cerr << exc.what() << std::endl;
			result = exc.getResult();
		}
		catch ( std::exception & exc )
		{
			std::cerr << exc.what() << std::endl;
		}
		catch ( ... )
		{
			std::cerr << "Unknown error" << std::endl;
		}

		return result;
	}

	VkResult DeviceMemory::bindToImage1D( VkImage image
		, VkDeviceSize memoryOffset
		, ObjectMemory *& objectMemory )
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;

		try
		{
			Texture1DDeviceMemory impl
			{
				m_device,
				get( this ),
				memoryOffset,
				m_allocateInfo,
				get( image )->getCreateInfo()
			};
			m_objects.emplace_back( std::make_unique< ObjectMemory >( std::move( impl.memory ) ) );
			objectMemory = m_objects.back().get();
			if ( m_mappedSize != 0u )
			{
				auto context{ get( m_device )->getImmediateContext() };
				updateUpload( context, *objectMemory, 0ULL, WholeSize, 0u );
			}
			result = VK_SUCCESS;
		}
		catch ( Exception & exc )
		{
			std::cerr << exc.what() << std::endl;
			result = exc.getResult();
		}
		catch ( std::exception & exc )
		{
			std::cerr << exc.what() << std::endl;
		}
		catch ( ... )
		{
			std::cerr << "Unknown error" << std::endl;
		}

		return result;
	}

	VkResult DeviceMemory::bindToImage2D( VkImage image
		, VkDeviceSize memoryOffset
		, ObjectMemory *& objectMemory )
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;

		auto const requirements = get( image )->getMemoryRequirements();
		if ( memoryOffset > m_allocateInfo.allocationSize
			|| requirements.size > m_allocateInfo.allocationSize - memoryOffset )
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}

		try
		{
			Texture2DDeviceMemory impl
			{
				m_device,
				get( this ),
				memoryOffset,
				m_allocateInfo,
				get( image )->getCreateInfo()
			};
			m_objects.emplace_back( std::make_unique< ObjectMemory >( std::move( impl.memory ) ) );
			objectMemory = m_objects.back().get();
			if ( m_mappedSize != 0u )
			{
				auto context{ get( m_device )->getImmediateContext() };
				updateUpload( context, *objectMemory, 0ULL, WholeSize, 0u );
			}
			result = VK_SUCCESS;
		}
		catch ( Exception & exc )
		{
			std::cerr << exc.what() << std::endl;
			result = exc.getResult();
		}
		catch ( std::exception & exc )
		{
			std::cerr << exc.what() << std::endl;
		}
		catch ( ... )
		{
			std::cerr << "Unknown error" << std::endl;
		}

		return result;
	}

	VkResult DeviceMemory::bindToImage3D( VkImage image
		, VkDeviceSize memoryOffset
		, ObjectMemory *& objectMemory )
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;

		try
		{
			Texture3DDeviceMemory impl
			{
				m_device,
				get( this ),
				memoryOffset,
				m_allocateInfo,
				get( image )->getCreateInfo()
			};
			m_objects.emplace_back( std::make_unique< ObjectMemory >( std::move( impl.memory ) ) );
			objectMemory = m_objects.back().get();
			if ( m_mappedSize != 0u )
			{
				auto context{ get( m_device )->getImmediateContext() };
				updateUpload( context, *objectMemory, 0ULL, WholeSize, 0u );
			}
			result = VK_SUCCESS;
		}
		catch ( Exception & exc )
		{
			std::cerr << exc.what() << std::endl;
			result = exc.getResult();
		}
		catch ( std::exception & exc )
		{
			std::cerr << exc.what() << std::endl;
		}
		catch ( ... )
		{
			std::cerr << "Unknown error" << std::endl;
		}

		return result;
	}

	void DeviceMemory::updateUpload( DeviceContextLock const & context
		, ObjectMemory const & memory
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		if ( !m_data.empty() && memory.resource )
		{
			// v13.48: copy only the pages the application wrote since the last upload.
			D3D11_RESOURCE_DIMENSION dimension{};
			memory.resource->GetType( &dimension );
			if ( m_data.watched() && dimension == D3D11_RESOURCE_DIMENSION_BUFFER )
			{
				size_t const begin = size_t( memory.offset + ( ( size == WholeSize && offset == 0u ) ? 0u : offset ) );
				size_t const objectEnd = size_t( std::min< VkDeviceSize >( memory.offset + ( memory.size ? memory.size : memory.allocateInfo.allocationSize ), m_data.size() ) );
				size_t const end = size == WholeSize ? objectEnd : size_t( std::min< VkDeviceSize >( begin + size, objectEnd ) );
				if ( begin >= end ) return;
				g_watchRequestedBytes += end - begin;
				// v13.69: a range that contains no whole page can never have its page reset by this
				// path (only whole pages inside the range are reset, see WatchedBytes::resetRange), so
				// once that page was written the write-watch query always answers "dirty". Waylith's
				// dynamic uniform windows (a few hundred bytes) are almost all like that: copy them
				// directly and save the GetWriteWatch system call (~2-5 us each).
				// RW_GPU_SUBPAGE_DIRECT=0 = query anyway.
				static bool const subpageDirect = envflags::on( "RW_GPU_SUBPAGE_DIRECT", true );
				if ( memory.everUploaded && subpageDirect )
				{
					size_t const page = WatchedBytes::pageSize();
					size_t const innerFirst = ( begin + page - 1u ) / page * page;
					size_t const innerLast = ( end >= m_data.size() ) ? ( m_data.size() + page - 1u ) / page * page : end / page * page;
					if ( innerFirst >= innerLast )
					{
						memory.upload( context, m_data.data(), subresource, begin, end - begin );
						g_watchCopiedBytes += end - begin;
						g_subpageDirect.fetch_add( 1u, std::memory_order_relaxed );
						return;
					}
				}
				if ( memory.everUploaded )
				{
					thread_local std::vector< std::pair< size_t, size_t > > runs;
					static LARGE_INTEGER const wf = []() { LARGE_INTEGER f{}; QueryPerformanceFrequency( &f ); return f; }();   // v13.69: once
					LARGE_INTEGER w0{}, w1{};
					QueryPerformanceCounter( &w0 );
					bool const tracked = m_data.takeDirtyRuns( begin, end, runs );
					QueryPerformanceCounter( &w1 );
					if ( wf.QuadPart ) g_watchQueryUs += uint64_t( double( w1.QuadPart - w0.QuadPart ) * 1e6 / double( wf.QuadPart ) );
					if ( tracked )
					{
						if ( runs.empty() ) { ++g_watchSkippedUploads; return; }
						for ( auto const & [runBegin, runEnd] : runs )
						{
							memory.upload( context, m_data.data(), subresource, runBegin, runEnd - runBegin );
							g_watchCopiedBytes += runEnd - runBegin;
						}
						return;
					}
				}
				memory.upload( context, m_data.data(), subresource, begin, end - begin );
				g_watchCopiedBytes += end - begin;
				memory.everUploaded = true;
				m_data.resetRange( begin, end );
				return;
			}
			memory.upload( context
				, m_data.data()
				, subresource
				, ( ( size == WholeSize && offset == 0u )
					? 0u
					: offset + memory.offset )
				, size );
		}
	}

	void DeviceMemory::updateDownload( DeviceContextLock const & context
		, ObjectMemory const & memory
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		if ( !m_data.empty() && memory.resource )
		{
			memory.download( context
				, m_data.data()
				, subresource
				, ( ( size == WholeSize && offset == 0u )
					? 0u
					: offset + memory.offset )
				, size );
			// v13.48: the bridge wrote these bytes itself (GPU -> shadow); they match the GPU.
			// Only safe to forget when this object has had its first complete upload.
			if ( memory.everUploaded )
			{
				size_t const begin = size_t( memory.offset + ( ( size == WholeSize && offset == 0u ) ? 0u : offset ) );
				size_t const objectEnd = size_t( std::min< VkDeviceSize >( memory.offset + ( memory.size ? memory.size : memory.allocateInfo.allocationSize ), m_data.size() ) );
				size_t const end = size == WholeSize ? objectEnd : size_t( std::min< VkDeviceSize >( begin + size, objectEnd ) );
				m_data.resetRange( begin, end );
			}
		}
	}

		void DeviceMemory::updateUpload( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size
			, UINT subresource )const
		{
			if ( offset >= m_allocateInfo.allocationSize ) return;
			size = std::min( size, m_allocateInfo.allocationSize - offset );
			if ( !m_data.empty() && !m_objects.empty() )
			{
				auto paramOffset = offset;
			auto paramSize = size;

			for ( auto & object : m_objects )
			{
				if ( !object->resource ) continue;
				auto objectOffset = std::max( paramOffset, object->offset ) - object->offset;
				auto objectSize = std::min( paramSize
					, ( object->allocateInfo.allocationSize > objectOffset
						? object->allocateInfo.allocationSize - objectOffset
						: 0u ) );
				objectOffset = object->offset;

				if ( objectSize > 0u
					&& paramOffset + paramSize > objectOffset )
				{
					object->upload( context
						, m_data.data()
						, subresource
						, offset
						, size );
				}
			}

			m_dirty = false;
		}
	}

		void DeviceMemory::updateDownload( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size
			, UINT subresource )const
		{
			if ( offset >= m_allocateInfo.allocationSize ) return;
			size = std::min( size, m_allocateInfo.allocationSize - offset );
			if ( !m_data.empty() && !m_objects.empty() )
			{
				auto paramOffset = offset;
			auto paramSize = size;

			for ( auto & object : m_objects )
			{
				if ( !object->resource ) continue;
				auto objectOffset = std::max( paramOffset, object->offset ) - object->offset;
				auto objectSize = std::min( paramSize
					, ( object->allocateInfo.allocationSize > objectOffset
						? object->allocateInfo.allocationSize - objectOffset
						: 0u ) );
				objectOffset = object->offset;

				if ( objectSize > 0u
					&& paramOffset + paramSize > objectOffset )
				{
					object->download( context
						, m_data.data()
						, subresource
						, offset
						, size );
				}
			}

			m_dirty = false;
		}
	}

	void DeviceMemory::updateData( VkDeviceMemory src
		, VkDeviceSize srcOffset
		, VkDeviceSize dstOffset
		, VkDeviceSize size )
	{
		if ( !m_data.empty()
			&& !get( src )->m_data.empty() )
		{
			std::memcpy( &m_data[dstOffset]
				, &get( src )->m_data[srcOffset]
				, size );
		}
	}

		VkResult DeviceMemory::lock( VkDeviceSize offset
			, VkDeviceSize size
			, VkMemoryMapFlags flags
			, void ** data )const
		{
			if ( data ) *data = nullptr;
			if ( !m_device || get( m_device )->getStatus() != VK_SUCCESS )
			{
				return VK_ERROR_DEVICE_LOST;
			}
			if ( !data ) return VK_ERROR_MEMORY_MAP_FAILED;
			assert( !m_mapped && "VkDeviceMemory should not be mapped" );
		assert( !m_data.empty() && "VkDeviceMemory should be mappable" );
		size = (size == WholeSize
			? m_allocateInfo.allocationSize
			: size);

		if ( m_data.size() <= offset 
			|| m_data.size() < ( offset + size ) )
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}

		*data = m_data.data() + offset;
		m_mappedOffset = offset;
		m_mappedSubresource = flags;
		m_mappedSize = size;
		m_mapped = *data != nullptr;
		m_dirty = true;
		return VK_SUCCESS;
	}

		VkResult DeviceMemory::flush( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size )const
		{
			if ( !m_device || get( m_device )->getStatus() != VK_SUCCESS ) return VK_ERROR_DEVICE_LOST;
			assert( m_mapped && "VkDeviceMemory should be mapped" );
		assert( !m_data.empty() && "VkDeviceMemory should be mappable" );
		size = ( size == WholeSize
			? m_allocateInfo.allocationSize
			: size );

		if ( m_data.size() <= offset
			|| m_data.size() < ( offset + size ) )
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}

		updateUpload( context, offset, size, m_mappedSubresource );
		return VK_SUCCESS;
	}

		VkResult DeviceMemory::invalidate( DeviceContextLock const & context
			, VkDeviceSize offset
			, VkDeviceSize size )const
		{
			if ( !m_device || get( m_device )->getStatus() != VK_SUCCESS ) return VK_ERROR_DEVICE_LOST;
			assert( m_mapped && "VkDeviceMemory should be mapped" );
		assert( !m_data.empty() && "VkDeviceMemory should be mappable" );
		size = ( size == WholeSize
			? m_allocateInfo.allocationSize
			: size );

		if ( m_data.size() <= offset
			|| m_data.size() < ( offset + size ) )
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}

		m_dirty = true;
		updateDownload( context, offset, size, m_mappedSubresource );
		return VK_SUCCESS;
	}

	void DeviceMemory::unlock( DeviceContextLock const & context )const
	{
		assert( m_mapped && "VkDeviceMemory should be mapped" );
		assert( !m_data.empty() && "VkDeviceMemory should be mappable" );
		m_mapped = false;
		updateUpload( context, m_mappedOffset, m_mappedSize, m_mappedSubresource );
	}

	void DeviceMemory::upload( DeviceContextLock const & context
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		updateUpload( context, offset, size, subresource );
	}

	void DeviceMemory::download( DeviceContextLock const & context
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		updateDownload( context, offset, size, subresource );
	}

	//*********************************************************************************************
}
