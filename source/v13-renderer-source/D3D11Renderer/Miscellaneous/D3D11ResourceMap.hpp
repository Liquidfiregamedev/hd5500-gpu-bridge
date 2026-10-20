/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

#include <atomic>
#include <cstdio>

namespace ashes::D3D11_NAMESPACE::resmap
{
	namespace detail
	{
		struct ResourceDesc
		{
			D3D11_RESOURCE_DIMENSION dimension{ D3D11_RESOURCE_DIMENSION_UNKNOWN };
			D3D11_USAGE usage{};
			UINT cpuAccess{};
			UINT bind{};
			UINT misc{};
			UINT byteWidth{};
			UINT width{};
			UINT height{ 1u };
			UINT depth{ 1u };
			UINT mipLevels{ 1u };
			UINT arraySize{ 1u };
			UINT sampleCount{ 1u };
			UINT sampleQuality{};
			DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
		};

		struct DeviceRef
		{
			ID3D11Device * value{};

			~DeviceRef()noexcept
			{
				if ( value )
				{
					value->Release();
				}
			}
		};

		inline void trace( char const * stage
			, HRESULT hr
			, ResourceDesc const & desc
			, UINT subresource
				, D3D11_MAP map )noexcept
			{
				std::fprintf( stderr,
					"[RW-GPU-MAP] %s hr=0x%08lX dim=%u sub=%u map=%u usage=%u cpu=0x%X bind=0x%X misc=0x%X format=%u bytes=%u size=%ux%ux%u mips=%u array=%u samples=%u:%u\n",
					stage,
					static_cast< unsigned long >( hr ),
					unsigned( desc.dimension ),
				subresource,
				unsigned( map ),
				unsigned( desc.usage ),
				desc.cpuAccess,
				desc.bind,
				desc.misc,
				unsigned( desc.format ),
				desc.byteWidth,
				desc.width,
				desc.height,
				desc.depth,
				desc.mipLevels,
				desc.arraySize,
				desc.sampleCount,
				desc.sampleQuality );
			std::fflush( stderr );
		}

		inline bool isSubresource( ResourceDesc const & desc, UINT subresource )noexcept
		{
			uint64_t count{};

			switch ( desc.dimension )
			{
			case D3D11_RESOURCE_DIMENSION_BUFFER:
				count = 1u;
				break;
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
				count = uint64_t( desc.mipLevels ) * desc.arraySize;
				break;
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				count = desc.mipLevels;
				break;
			default:
				return false;
			}

			return uint64_t( subresource ) < count;
		}

		inline UINT requiredCpuAccess( D3D11_MAP map )noexcept
		{
			switch ( map )
			{
			case D3D11_MAP_READ:
				return D3D11_CPU_ACCESS_READ;
			case D3D11_MAP_WRITE:
				return D3D11_CPU_ACCESS_WRITE;
			case D3D11_MAP_READ_WRITE:
				return D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
			default:
				return 0u;
			}
		}

		inline bool isDirectStagingMap( ResourceDesc const & desc, D3D11_MAP map )noexcept
		{
			auto const required = requiredCpuAccess( map );
			return desc.usage == D3D11_USAGE_STAGING
				&& required != 0u
				&& ( desc.cpuAccess & required ) == required;
		}

		inline bool isDirectDynamicMap( ResourceDesc const & desc, D3D11_MAP map )noexcept
		{
			// WRITE_DISCARD is the map request this renderer emits for dynamic,
			// pipeline-bound buffers.  Other modes are deliberately staged rather
			// than relying on implementation-specific D3D11 Map acceptance.
			return desc.usage == D3D11_USAGE_DYNAMIC
				&& map == D3D11_MAP_WRITE_DISCARD
				&& ( desc.cpuAccess & D3D11_CPU_ACCESS_WRITE ) != 0u;
		}

		inline bool canCopyBack( ResourceDesc const & desc )noexcept
		{
			// An immutable resource cannot be the destination of the copy made by
			// unlock().  It has no legal writable map path either.
			return desc.usage != D3D11_USAGE_IMMUTABLE;
		}

		inline void traceFallback( ResourceDesc const & desc
			, UINT subresource
			, D3D11_MAP map )noexcept
		{
			static std::atomic< unsigned long > count{ 0u };
			auto const ordinal = count.fetch_add( 1u, std::memory_order_relaxed ) + 1u;

				if ( ordinal <= 32u )
				{
					std::fprintf( stderr,
						"[RW-GPU-MAP] fallback #%lu dim=%u sub=%u requestedMap=%u usage=%u cpu=0x%X bind=0x%X misc=0x%X format=%u bytes=%u size=%ux%ux%u mips=%u array=%u samples=%u:%u\n",
						ordinal,
						unsigned( desc.dimension ),
						subresource,
					unsigned( map ),
					unsigned( desc.usage ),
					desc.cpuAccess,
					desc.bind,
					desc.misc,
					unsigned( desc.format ),
					desc.byteWidth,
					desc.width,
					desc.height,
					desc.depth,
					desc.mipLevels,
					desc.arraySize,
					desc.sampleCount,
					desc.sampleQuality );
				std::fflush( stderr );
			}
		}

		inline HRESULT createStaging( ID3D11Device * device
			, ID3D11Resource * source
			, ResourceDesc & resourceDesc
			, ID3D11Resource *& result )noexcept
		{
			result = nullptr;
			D3D11_RESOURCE_DIMENSION dimension{ D3D11_RESOURCE_DIMENSION_UNKNOWN };
			source->GetType( &dimension );
			resourceDesc.dimension = dimension;

			switch ( dimension )
			{
			case D3D11_RESOURCE_DIMENSION_BUFFER:
				{
					auto buffer = static_cast< ID3D11Buffer * >( source );
					D3D11_BUFFER_DESC desc{};
					buffer->GetDesc( &desc );
					resourceDesc.usage = desc.Usage;
					resourceDesc.cpuAccess = desc.CPUAccessFlags;
					resourceDesc.bind = desc.BindFlags;
					resourceDesc.misc = desc.MiscFlags;
					resourceDesc.byteWidth = desc.ByteWidth;
					resourceDesc.width = desc.ByteWidth;
					desc.Usage = D3D11_USAGE_STAGING;
					desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
					desc.BindFlags = 0u;
					desc.MiscFlags = 0u;
					desc.StructureByteStride = 0u;
					ID3D11Buffer * staging{};
					auto const hr = device->CreateBuffer( &desc, nullptr, &staging );
					result = staging;
					return hr;
				}
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				{
					auto texture = static_cast< ID3D11Texture1D * >( source );
					D3D11_TEXTURE1D_DESC desc{};
					texture->GetDesc( &desc );
					resourceDesc.usage = desc.Usage;
					resourceDesc.cpuAccess = desc.CPUAccessFlags;
					resourceDesc.bind = desc.BindFlags;
					resourceDesc.misc = desc.MiscFlags;
					resourceDesc.width = desc.Width;
					resourceDesc.mipLevels = desc.MipLevels;
					resourceDesc.arraySize = desc.ArraySize;
					resourceDesc.format = desc.Format;
					desc.Usage = D3D11_USAGE_STAGING;
					desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
					desc.BindFlags = 0u;
					desc.MiscFlags = 0u;
					ID3D11Texture1D * staging{};
					auto const hr = device->CreateTexture1D( &desc, nullptr, &staging );
					result = staging;
					return hr;
				}
			case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
				{
					auto texture = static_cast< ID3D11Texture2D * >( source );
					D3D11_TEXTURE2D_DESC desc{};
					texture->GetDesc( &desc );
					resourceDesc.usage = desc.Usage;
					resourceDesc.cpuAccess = desc.CPUAccessFlags;
					resourceDesc.bind = desc.BindFlags;
					resourceDesc.misc = desc.MiscFlags;
					resourceDesc.width = desc.Width;
					resourceDesc.height = desc.Height;
					resourceDesc.mipLevels = desc.MipLevels;
					resourceDesc.arraySize = desc.ArraySize;
					resourceDesc.sampleCount = desc.SampleDesc.Count;
					resourceDesc.sampleQuality = desc.SampleDesc.Quality;
					resourceDesc.format = desc.Format;

					// D3D11 does not permit multisampled STAGING textures.  Do not
					// silently construct a non-equivalent single-sample companion.
					if ( desc.SampleDesc.Count > 1u )
					{
						return E_NOTIMPL;
					}

					desc.Usage = D3D11_USAGE_STAGING;
					desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
					desc.BindFlags = 0u;
					desc.MiscFlags = 0u;
					ID3D11Texture2D * staging{};
					auto const hr = device->CreateTexture2D( &desc, nullptr, &staging );
					result = staging;
					return hr;
				}
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				{
					auto texture = static_cast< ID3D11Texture3D * >( source );
					D3D11_TEXTURE3D_DESC desc{};
					texture->GetDesc( &desc );
					resourceDesc.usage = desc.Usage;
					resourceDesc.cpuAccess = desc.CPUAccessFlags;
					resourceDesc.bind = desc.BindFlags;
					resourceDesc.misc = desc.MiscFlags;
					resourceDesc.width = desc.Width;
					resourceDesc.height = desc.Height;
					resourceDesc.depth = desc.Depth;
					resourceDesc.mipLevels = desc.MipLevels;
					resourceDesc.format = desc.Format;
					desc.Usage = D3D11_USAGE_STAGING;
					desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
					desc.BindFlags = 0u;
					desc.MiscFlags = 0u;
					ID3D11Texture3D * staging{};
					auto const hr = device->CreateTexture3D( &desc, nullptr, &staging );
					result = staging;
					return hr;
				}
			default:
				return E_NOTIMPL;
			}
		}
	}

	/**
	 * Maps an ObjectMemory native subresource without ever issuing an illegal
	 * Map against a DEFAULT resource.  A transient STAGING resource is retained
	 * in ObjectMemory only between this call and unlock().
	 */
	inline HRESULT lock( ObjectMemory const & memory
		, ID3D11DeviceContext * context
		, UINT nativeSubresource
		, D3D11_MAPPED_SUBRESOURCE & data )
	{
		data = {};
		detail::ResourceDesc desc{};

		if ( !memory.resource || !context )
		{
			detail::trace( "lock-invalid-argument", E_POINTER, desc, nativeSubresource, memory.mapFlags );
			return E_POINTER;
		}

		memory.resource->GetType( &desc.dimension );
		if ( memory.mapStaging )
		{
			detail::trace( "lock-already-staged", E_FAIL, desc, nativeSubresource, memory.mapFlags );
			return E_FAIL;
		}

		// createStaging reads the exact descriptor before cloning it.  It is also
		// used for direct-map classification so no map decision is inferred from
		// Vulkan allocation flags alone.
		ID3D11Resource * staging{};
		detail::DeviceRef device;
		memory.resource->GetDevice( &device.value );
		if ( !device.value )
		{
			detail::trace( "lock-no-device", E_FAIL, desc, nativeSubresource, memory.mapFlags );
			return E_FAIL;
		}

		// Populate the descriptor without allocating: clone construction happens
		// below only when direct Map is illegal.  A tiny probe is avoided because
		// a helper must not allocate a staging object for a direct map.
		switch ( desc.dimension )
		{
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			{
				D3D11_BUFFER_DESC source{};
				static_cast< ID3D11Buffer * >( memory.resource )->GetDesc( &source );
				desc.usage = source.Usage;
				desc.cpuAccess = source.CPUAccessFlags;
				desc.bind = source.BindFlags;
				desc.misc = source.MiscFlags;
				desc.byteWidth = source.ByteWidth;
				desc.width = source.ByteWidth;
				break;
			}
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			{
				D3D11_TEXTURE1D_DESC source{};
				static_cast< ID3D11Texture1D * >( memory.resource )->GetDesc( &source );
				desc.usage = source.Usage;
				desc.cpuAccess = source.CPUAccessFlags;
				desc.bind = source.BindFlags;
				desc.misc = source.MiscFlags;
				desc.width = source.Width;
				desc.mipLevels = source.MipLevels;
				desc.arraySize = source.ArraySize;
				desc.format = source.Format;
				break;
			}
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			{
				D3D11_TEXTURE2D_DESC source{};
				static_cast< ID3D11Texture2D * >( memory.resource )->GetDesc( &source );
				desc.usage = source.Usage;
				desc.cpuAccess = source.CPUAccessFlags;
				desc.bind = source.BindFlags;
				desc.misc = source.MiscFlags;
				desc.width = source.Width;
				desc.height = source.Height;
				desc.mipLevels = source.MipLevels;
				desc.arraySize = source.ArraySize;
				desc.sampleCount = source.SampleDesc.Count;
				desc.sampleQuality = source.SampleDesc.Quality;
				desc.format = source.Format;
				break;
			}
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			{
				D3D11_TEXTURE3D_DESC source{};
				static_cast< ID3D11Texture3D * >( memory.resource )->GetDesc( &source );
				desc.usage = source.Usage;
				desc.cpuAccess = source.CPUAccessFlags;
				desc.bind = source.BindFlags;
				desc.misc = source.MiscFlags;
				desc.width = source.Width;
				desc.height = source.Height;
				desc.depth = source.Depth;
				desc.mipLevels = source.MipLevels;
				desc.format = source.Format;
				break;
			}
		default:
			detail::trace( "lock-unsupported-dimension", E_NOTIMPL, desc, nativeSubresource, memory.mapFlags );
			return E_NOTIMPL;
		}

		if ( !detail::isSubresource( desc, nativeSubresource ) )
		{
			detail::trace( "lock-invalid-subresource", E_INVALIDARG, desc, nativeSubresource, memory.mapFlags );
			return E_INVALIDARG;
		}

		if ( detail::isDirectStagingMap( desc, memory.mapFlags )
				|| detail::isDirectDynamicMap( desc, memory.mapFlags ) )
			{
				auto const hr = context->Map( memory.resource, nativeSubresource, memory.mapFlags, 0u, &data );
				if ( FAILED( hr ) || !data.pData )
				{
					auto const failure = FAILED( hr ) ? hr : E_FAIL;
					if ( SUCCEEDED( hr ) ) context->Unmap( memory.resource, nativeSubresource );
					data = {};
					detail::trace( "lock-direct-map", failure, desc, nativeSubresource, memory.mapFlags );
					return failure;
				}
				return hr;
			}

		if ( !detail::canCopyBack( desc ) )
		{
			detail::trace( "lock-immutable-unsupported", E_NOTIMPL, desc, nativeSubresource, memory.mapFlags );
			return E_NOTIMPL;
		}

		detail::traceFallback( desc, nativeSubresource, memory.mapFlags );
		auto hr = detail::createStaging( device.value, memory.resource, desc, staging );
		if ( FAILED( hr ) || !staging )
		{
			auto const failure = FAILED( hr ) ? hr : E_FAIL;
			detail::trace( "lock-create-staging", failure, desc, nativeSubresource, memory.mapFlags );
			if ( staging )
			{
				staging->Release();
			}
			return failure;
		}

		// Copy only the caller's native subresource.  That deliberately retains
		// the renderer's mip-prefix/native-index convention unchanged.
		context->CopySubresourceRegion( staging, nativeSubresource, 0u, 0u, 0u
			, memory.resource, nativeSubresource, nullptr );
		hr = context->Map( staging, nativeSubresource, D3D11_MAP_READ_WRITE, 0u, &data );
		if ( FAILED( hr ) || !data.pData )
		{
			auto const failure = FAILED( hr ) ? hr : E_FAIL;
			if ( SUCCEEDED( hr ) ) context->Unmap( staging, nativeSubresource );
			data = {};
			detail::trace( "lock-stage-map", failure, desc, nativeSubresource, D3D11_MAP_READ_WRITE );
			staging->Release();
			return failure;
		}

		memory.mapStaging = staging;
		return hr;
	}

	/**
	 * Completes a lock().  Transient staging resources are copied back only for
	 * the native subresource that the caller supplies, then immediately released.
	 */
	inline void unlock( ObjectMemory const & memory
		, ID3D11DeviceContext * context
		, UINT nativeSubresource )
	{
		if ( !context )
		{
			detail::ResourceDesc desc{};
			detail::trace( "unlock-invalid-context", E_POINTER, desc, nativeSubresource, memory.mapFlags );
			return;
		}

		if ( memory.mapStaging )
		{
			auto staging = memory.mapStaging;
			context->Unmap( staging, nativeSubresource );
			if ( memory.resource )
			{
				context->CopySubresourceRegion( memory.resource, nativeSubresource, 0u, 0u, 0u
					, staging, nativeSubresource, nullptr );
			}
			else
			{
				detail::ResourceDesc desc{};
				detail::trace( "unlock-missing-source", E_POINTER, desc, nativeSubresource, memory.mapFlags );
			}
			memory.mapStaging = nullptr;
			staging->Release();
			return;
		}

		if ( memory.resource )
		{
			context->Unmap( memory.resource, nativeSubresource );
		}
	}
}
