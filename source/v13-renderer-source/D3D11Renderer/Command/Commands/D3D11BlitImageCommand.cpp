/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11BlitImageCommand.hpp"
#include "Command/Commands/D3D11BindDescriptorSetCommand.hpp"

#include "Command/D3D11CommandBuffer.hpp"
#include "Core/D3D11Device.hpp"
#include "Descriptor/D3D11DescriptorSetLayout.hpp"
#include "Image/D3D11Image.hpp"
#include "Image/D3D11ImageView.hpp"
#include "Miscellaneous/D3D11BlitScratch.hpp"
#include "Pipeline/D3D11Pipeline.hpp"
#include "Pipeline/D3D11PipelineLayout.hpp"
#include "RenderPass/D3D11FrameBuffer.hpp"

#include "ashesd3d11_api.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

namespace
{
	bool isHd5500BlitTraceEnabled()noexcept
	{
		static bool const enabled = std::getenv( "RW_HD5500_BLIT_TRACE" ) != nullptr;
		return enabled;
	}

	bool isHd5500BlitScratchTraceEnabled()noexcept
	{
		static bool const enabled = []()
		{
			auto const value = std::getenv( "RW_HD5500_BLIT_SCRATCH_TRACE" );
			return value && value[0] == '1' && value[1] == '\0';
		}();
		return enabled;
	}

	struct BlitScratchTraceState
	{
		std::mutex mutex;
		uint64_t allocationCount{};
		uint64_t liveBytes{};
		uint64_t peakBytes{};
		uint32_t fallbackLogs{};
		uint32_t failureLogs{};
		bool loggedFinalZero{};
	};

	BlitScratchTraceState & getBlitScratchTraceState()noexcept
	{
		static BlitScratchTraceState result;
		return result;
	}

	void logBlitScratch( char const * event
		, uint64_t allocationCount
		, uint64_t id
		, VkExtent3D const & atlasExtent
		, VkExtent3D const & scratchExtent
		, VkOffset3D const & bboxOrigin
		, uint32_t mipLevel
		, uint32_t baseArrayLayer
		, uint32_t layerCount
		, uint64_t payloadBytes
		, uint64_t liveBytes
		, uint64_t peakBytes
		, char const * mode
		, char const * reason
		, char const * stage = nullptr )noexcept
	{
		std::fprintf( stderr,
			"[RW-GPU-BLIT-SCRATCH] event=%s allocationCount=%llu id=%llu atlas=%ux%ux%u scratch=%ux%ux%u bbox=%d,%d mip=%u layers=%u+%u payloadBytes=%llu liveBytes=%llu peakBytes=%llu mode=%s reason=%s%s%s\n",
			event,
			static_cast< unsigned long long >( allocationCount ),
			static_cast< unsigned long long >( id ),
			atlasExtent.width, atlasExtent.height, atlasExtent.depth,
			scratchExtent.width, scratchExtent.height, scratchExtent.depth,
			bboxOrigin.x, bboxOrigin.y, mipLevel, baseArrayLayer, layerCount,
			static_cast< unsigned long long >( payloadBytes ),
			static_cast< unsigned long long >( liveBytes ),
			static_cast< unsigned long long >( peakBytes ),
			mode, reason, stage ? " stage=" : "", stage ? stage : "" );
		std::fflush( stderr );
	}

	uint64_t recordCompactScratchAllocation( VkExtent3D const & atlasExtent
		, VkExtent3D const & scratchExtent
		, VkOffset3D const & bboxOrigin
		, uint32_t mipLevel
		, uint32_t baseArrayLayer
		, uint32_t layerCount
		, uint64_t payloadBytes )noexcept
	{
		auto & state = getBlitScratchTraceState();
		std::lock_guard< std::mutex > lock{ state.mutex };
		auto const id = ++state.allocationCount;
		state.liveBytes = payloadBytes > ( std::numeric_limits< uint64_t >::max )() - state.liveBytes
			? ( std::numeric_limits< uint64_t >::max )()
			: state.liveBytes + payloadBytes;
		state.peakBytes = ( std::max )( state.peakBytes, state.liveBytes );
		if ( isHd5500BlitScratchTraceEnabled() && id <= 256u )
		{
			logBlitScratch( "alloc", state.allocationCount, id, atlasExtent, scratchExtent, bboxOrigin
				, mipLevel, baseArrayLayer, layerCount, payloadBytes, state.liveBytes, state.peakBytes
				, "compact", "common-mip-bbox" );
		}
		return id;
	}

	void recordCompactScratchRelease( uint64_t id
		, VkExtent3D const & atlasExtent
		, VkExtent3D const & scratchExtent
		, VkOffset3D const & bboxOrigin
		, uint32_t mipLevel
		, uint32_t baseArrayLayer
		, uint32_t layerCount
		, uint64_t payloadBytes )noexcept
	{
		auto & state = getBlitScratchTraceState();
		std::lock_guard< std::mutex > lock{ state.mutex };
		state.liveBytes = state.liveBytes >= payloadBytes ? state.liveBytes - payloadBytes : 0u;
		auto const isFinalZero = state.liveBytes == 0u && !state.loggedFinalZero;
		if ( isFinalZero ) state.loggedFinalZero = true;
		if ( isHd5500BlitScratchTraceEnabled() && ( id <= 256u || isFinalZero ) )
		{
			logBlitScratch( "release", state.allocationCount, id, atlasExtent, scratchExtent, bboxOrigin
				, mipLevel, baseArrayLayer, layerCount, payloadBytes, state.liveBytes, state.peakBytes
				, "compact", "common-mip-bbox", isFinalZero ? "final-release-zero" : nullptr );
		}
	}

	void recordScratchFallbackOrFailure( char const * event
		, VkExtent3D const & atlasExtent
		, VkExtent3D const & scratchExtent
		, VkOffset3D const & bboxOrigin
		, uint32_t mipLevel
		, uint32_t baseArrayLayer
		, uint32_t layerCount
		, char const * mode
		, char const * reason
		, char const * stage = nullptr )noexcept
	{
		if ( !isHd5500BlitScratchTraceEnabled() ) return;
		auto & state = getBlitScratchTraceState();
		std::lock_guard< std::mutex > lock{ state.mutex };
		auto & logs = stage ? state.failureLogs : state.fallbackLogs;
		if ( logs++ >= 32u ) return;
		logBlitScratch( event, state.allocationCount, 0u, atlasExtent, scratchExtent, bboxOrigin
			, mipLevel, baseArrayLayer, layerCount, 0u, state.liveBytes, state.peakBytes, mode, reason, stage );
	}
}

namespace ashes::D3D11_NAMESPACE
{
	//*********************************************************************************************
	
	namespace blitimg
	{
		static std::string getImgFmtName( VkFormat fmt )
		{
			switch ( fmt )
			{
			case VK_FORMAT_R8_UNORM:
			case VK_FORMAT_R8_SNORM:
			case VK_FORMAT_R8_USCALED:
			case VK_FORMAT_R8_SSCALED:
			case VK_FORMAT_R8_UINT:
			case VK_FORMAT_R8_SINT:
			case VK_FORMAT_R8_SRGB:
			case VK_FORMAT_R16_UNORM:
			case VK_FORMAT_R16_SNORM:
			case VK_FORMAT_R16_USCALED:
			case VK_FORMAT_R16_SSCALED:
			case VK_FORMAT_R16_UINT:
			case VK_FORMAT_R16_SINT:
			case VK_FORMAT_R16_SFLOAT:
			case VK_FORMAT_R32_UINT:
			case VK_FORMAT_R32_SINT:
			case VK_FORMAT_R32_SFLOAT:
			case VK_FORMAT_R64_UINT:
			case VK_FORMAT_R64_SINT:
			case VK_FORMAT_R64_SFLOAT:
			case VK_FORMAT_D16_UNORM:
			case VK_FORMAT_D32_SFLOAT:
			case VK_FORMAT_S8_UINT:
			case VK_FORMAT_D16_UNORM_S8_UINT:
			case VK_FORMAT_R10X6_UNORM_PACK16:
			case VK_FORMAT_R12X4_UNORM_PACK16:
				return "float";

			case VK_FORMAT_R4G4_UNORM_PACK8:
			case VK_FORMAT_R8G8_UNORM:
			case VK_FORMAT_R8G8_SNORM:
			case VK_FORMAT_R8G8_USCALED:
			case VK_FORMAT_R8G8_SSCALED:
			case VK_FORMAT_R8G8_UINT:
			case VK_FORMAT_R8G8_SINT:
			case VK_FORMAT_R8G8_SRGB:
			case VK_FORMAT_R16G16_UNORM:
			case VK_FORMAT_R16G16_SNORM:
			case VK_FORMAT_R16G16_USCALED:
			case VK_FORMAT_R16G16_SSCALED:
			case VK_FORMAT_R16G16_UINT:
			case VK_FORMAT_R16G16_SINT:
			case VK_FORMAT_R16G16_SFLOAT:
			case VK_FORMAT_R32G32_UINT:
			case VK_FORMAT_R32G32_SINT:
			case VK_FORMAT_R32G32_SFLOAT:
			case VK_FORMAT_R64G64_UINT:
			case VK_FORMAT_R64G64_SINT:
			case VK_FORMAT_R64G64_SFLOAT:
			case VK_FORMAT_EAC_R11_UNORM_BLOCK:
			case VK_FORMAT_EAC_R11_SNORM_BLOCK:
			case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
			case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
				return "float2";

			case VK_FORMAT_R5G6B5_UNORM_PACK16:
			case VK_FORMAT_B5G6R5_UNORM_PACK16:
			case VK_FORMAT_R8G8B8_UNORM:
			case VK_FORMAT_R8G8B8_SNORM:
			case VK_FORMAT_R8G8B8_USCALED:
			case VK_FORMAT_R8G8B8_SSCALED:
			case VK_FORMAT_R8G8B8_UINT:
			case VK_FORMAT_R8G8B8_SINT:
			case VK_FORMAT_R8G8B8_SRGB:
			case VK_FORMAT_B8G8R8_UNORM:
			case VK_FORMAT_B8G8R8_SNORM:
			case VK_FORMAT_B8G8R8_USCALED:
			case VK_FORMAT_B8G8R8_SSCALED:
			case VK_FORMAT_B8G8R8_UINT:
			case VK_FORMAT_B8G8R8_SINT:
			case VK_FORMAT_B8G8R8_SRGB:
			case VK_FORMAT_R16G16B16_UNORM:
			case VK_FORMAT_R16G16B16_SNORM:
			case VK_FORMAT_R16G16B16_USCALED:
			case VK_FORMAT_R16G16B16_SSCALED:
			case VK_FORMAT_R16G16B16_UINT:
			case VK_FORMAT_R16G16B16_SINT:
			case VK_FORMAT_R16G16B16_SFLOAT:
			case VK_FORMAT_R32G32B32_UINT:
			case VK_FORMAT_R32G32B32_SINT:
			case VK_FORMAT_R32G32B32_SFLOAT:
			case VK_FORMAT_R64G64B64_UINT:
			case VK_FORMAT_R64G64B64_SINT:
			case VK_FORMAT_R64G64B64_SFLOAT:
			case VK_FORMAT_X8_D24_UNORM_PACK32:
			case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
			case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
			case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
			case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
			case VK_FORMAT_BC2_UNORM_BLOCK:
			case VK_FORMAT_BC2_SRGB_BLOCK:
			case VK_FORMAT_BC3_UNORM_BLOCK:
			case VK_FORMAT_BC3_SRGB_BLOCK:
			case VK_FORMAT_BC4_UNORM_BLOCK:
			case VK_FORMAT_BC4_SNORM_BLOCK:
			case VK_FORMAT_BC5_UNORM_BLOCK:
			case VK_FORMAT_BC5_SNORM_BLOCK:
			case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
			case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
			case VK_FORMAT_G8B8G8R8_422_UNORM:
			case VK_FORMAT_B8G8R8G8_422_UNORM:
			case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
			case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
			case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
			case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
			case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
			case VK_FORMAT_R10X6G10X6_UNORM_2PACK16:
			case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
			case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16:
			case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
			case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
			case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
			case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
			case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
			case VK_FORMAT_R12X4G12X4_UNORM_2PACK16:
			case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
			case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16:
			case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
			case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
			case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
			case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
			case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
			case VK_FORMAT_G16B16G16R16_422_UNORM:
			case VK_FORMAT_B16G16R16G16_422_UNORM:
			case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
			case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
			case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
			case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
			case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
				return "float3";

			default:
				return "float4";
			}
		}

		static ashes::UInt32Array getBlitShaderCode( VkFormat src
			, VkFormat dst )
		{
			// The command only creates this pipeline for float4 RGBA8 output.
			// Do not synthesize typed integer or sRGB UAV shaders for this bridge.
			static_cast< void >( src );
			static_cast< void >( dst );
			std::string const hlsl =
				R"(Texture2D<float4> srcTexture : register(t0);
	RWTexture2D<float4> dstTexture : register(u0);
	SamplerState blitSampler : register(s1); // Separate sampler is native tex binding 1; source SRV is t0.

	cbuffer CB : register(b0)
	{
		float4 srcBox;   // source pixel float4( origin, extent )
		float4 dstBox;   // destination pixel float4( origin, extent )
	}

	[numthreads( 8, 8, 1 )]
	void main( uint3 threadID : SV_DispatchThreadID )
	{
		uint2 local = threadID.xy;
		uint2 dstExtent = uint2( dstBox.zw );
		if ( local.x >= dstExtent.x || local.y >= dstExtent.y ) return;
		uint srcWidth, srcHeight;
		srcTexture.GetDimensions( srcWidth, srcHeight );
		float2 sourcePixel = srcBox.xy + ( float2( local ) + 0.5f ) * ( srcBox.zw / dstBox.zw );
		float2 srcTextcoords = sourcePixel / float2( srcWidth, srcHeight );
		int2 dstTextcoords = int2( dstBox.xy ) + int2( local );
		dstTexture[dstTextcoords] = srcTexture.SampleLevel( blitSampler, srcTextcoords, 0.0f );
	})";
			auto size = hlsl.size() + 1u;
			UInt32Array result;
			result.resize( size_t( std::ceil( float( size ) / sizeof( uint32_t ) ) ) );
			std::memcpy( result.data(), hlsl.data(), hlsl.size() );
			return result;
		}

		static VkDescriptorSetLayout doCreateBlitDescriptorLayout( Device const & device )
		{
			VkDescriptorSetLayoutBindingArray bindings
			{
				{ 0u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
				{ 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
				{ 2u, VK_DESCRIPTOR_TYPE_SAMPLER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
				{ 3u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			};
			VkDescriptorSetLayout result{};
			auto status = allocate( result
				, device.getAllocationCallbacks()
				, get( &device )
				, VkDescriptorSetLayoutCreateInfo
				{
					VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
					nullptr,
					0u,
					uint32_t( bindings.size() ),
					bindings.data()
				} );
			if ( status != VK_SUCCESS || !result ) throw Exception{ status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status, "doCreateBlitDescriptorLayout failed" };
			return result;
		}

		static VkPipelineLayout doCreateBlitPipelineLayout( Device const & device
			, VkDescriptorSetLayout descriptorLayout )
		{
			VkPipelineLayout result{};
			auto status = allocate( result
				, device.getAllocationCallbacks()
				, get( &device )
				, VkPipelineLayoutCreateInfo
				{
					VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
					nullptr,
					0u,
					1u,
					&descriptorLayout,
					0u,
					nullptr
				} );
			if ( status != VK_SUCCESS || !result ) throw Exception{ status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status, "doCreateBlitPipelineLayout failed" };
			return result;
		}

		static VkShaderModule doCreateShaderModule( Device const & device
			, VkFormat src
			, VkFormat dst
			, UInt32Array & spirv )
		{
			spirv = getBlitShaderCode( src, dst );
			VkShaderModule result{};
			auto status = allocate( result
				, device.getAllocationCallbacks()
				, get( &device )
				, VkShaderModuleCreateInfo
				{
					VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
					nullptr,
					0u,
					spirv.size() * sizeof( uint32_t ),
					spirv.data()
				} );
			if ( status != VK_SUCCESS || !result ) throw Exception{ status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status, "doCreateShaderModule failed" };
			return result;
		}

		static VkPipeline doCreateBlitPipeline( Device const & device
			, VkPipelineLayout pipelineLayout
			, VkShaderModule shaderModule )
		{
			VkPipeline result{};
			auto status = allocate( result
				, device.getAllocationCallbacks()
				, get( &device )
				, VkComputePipelineCreateInfo
				{
					VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
					nullptr,
					0u,
					VkPipelineShaderStageCreateInfo
					{
						VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
						nullptr,
						0u,
						VK_SHADER_STAGE_COMPUTE_BIT,
						shaderModule,
						"main",
						nullptr,
					},
					pipelineLayout,
					nullptr,
					0u,
				} );
			if ( status != VK_SUCCESS || !result ) throw Exception{ status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status, "doCreateBlitPipeline failed" };
			return result;
		}

		static VkSampler doCreateSampler( VkDevice device
			, VkFilter filter
			, uint32_t mipLevels )
		{
			VkSampler result{};
			auto status = allocate( result
				, get( device )->getAllocationCallbacks()
				, device
				, VkSamplerCreateInfo
				{
					VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
					nullptr,
					0u,
					filter,
					filter,
					VK_SAMPLER_MIPMAP_MODE_LINEAR,
					VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
					VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
					VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
					0.0f,
					VK_FALSE,
					0.0f,
					VK_FALSE,
					VK_COMPARE_OP_ALWAYS,
					0.0f,
					float( mipLevels ),
					VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
					VK_FALSE
				} );
				if ( status != VK_SUCCESS || !result )
				{
					throw Exception{ status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status, "Blit sampler allocation failed" };
				}
				return result;
			}

			static std::shared_ptr< BlitSamplerOwner > doGetCachedSampler( BlitPipeline const & pipeline
				, VkFilter filter )
			{
				std::weak_ptr< BlitSamplerOwner > * cache{};
				if ( filter == VK_FILTER_NEAREST ) cache = &pipeline.nearestSampler;
				else if ( filter == VK_FILTER_LINEAR ) cache = &pipeline.linearSampler;

				if ( !cache )
				{
					auto result = std::make_shared< BlitSamplerOwner >();
					result->device = pipeline.device;
					result->sampler = doCreateSampler( pipeline.device, filter, 1u );
					return result;
				}

				std::lock_guard< std::mutex > lock{ pipeline.samplerMutex };
				if ( auto result = cache->lock() ) return result;

				auto result = std::make_shared< BlitSamplerOwner >();
				result->device = pipeline.device;
				result->sampler = doCreateSampler( pipeline.device, filter, 1u );
				*cache = result;
				return result;
			}

			static VkBuffer doCreateUbo( VkDevice device
			, uint32_t count
			, VkDeviceMemory & memory
			, VkDeviceSize & range )
		{
			VkBuffer result{};
			auto status = allocate( result
				, get( device )->getAllocationCallbacks()
				, device
				, VkBufferCreateInfo
				{
					VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					nullptr,
					0u,
					count * 256u,
					VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					VK_SHARING_MODE_EXCLUSIVE,
					0u,
					nullptr,
				} );
			if ( status != VK_SUCCESS || !result )
			{
				throw Exception{ status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status, "Blit UBO allocation failed" };
			}

			if ( result != nullptr )
			{
				auto requirements = get( result )->getMemoryRequirements();
				range = requirements.alignment;
				uint32_t deduced = deduceMemoryType( requirements.memoryTypeBits
					, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT );

				status = allocate( memory
					, get( device )->getAllocationCallbacks()
					, device
					, VkMemoryAllocateInfo
					{
						VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
						nullptr,
						count * range,
						deduced,
					} );
				if ( status != VK_SUCCESS || !memory )
				{
					deallocate( result, get( device )->getAllocationCallbacks() );
					result = nullptr;
					memory = nullptr;
					throw Exception{ status == VK_SUCCESS ? VK_ERROR_OUT_OF_DEVICE_MEMORY : status, "Blit UBO memory allocation failed" };
				}
				status = get( result )->bindMemory( memory, 0u );
				if ( status != VK_SUCCESS )
				{
					deallocate( result, get( device )->getAllocationCallbacks() );
					result = nullptr;
					deallocate( memory, get( device )->getAllocationCallbacks() );
					memory = nullptr;
					throw Exception{ status, "Blit UBO memory bind failed" };
				}
			}

			return result;
		}

		static VkDescriptorSet doCreateDescriptorSet( VkDevice device
			, VkDescriptorPool pool
			, VkDescriptorSetLayout descriptorLayout
			, VkBuffer ubo
			, VkDeviceSize offset
			, VkDeviceSize range
			, VkSampler sampler
			, VkImageView srcView
			, VkImageView dstView )
		{
			VkDescriptorSet result{};
			auto status = allocateNA( result
				, device
				, pool
				, descriptorLayout );

			if ( status != VK_SUCCESS || !result ) throw Exception{ status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status, "Blit descriptor-set allocation failed" };
			if ( result )
			{
				VkDescriptorBufferInfo bufferInfo
				{
					ubo,
					offset,
					range,
				};
				get( result )->update( VkWriteDescriptorSet
					{
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						nullptr,
						result,
						3u,
						0u,
						1u,
						VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
						nullptr,
						&bufferInfo,
						nullptr
					} );
				VkDescriptorImageInfo srcSampler
				{
					sampler,
					nullptr,
					VK_IMAGE_LAYOUT_UNDEFINED,
				};
				get( result )->update( VkWriteDescriptorSet
					{
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						nullptr,
						result,
						2u,
						0u,
						1u,
						VK_DESCRIPTOR_TYPE_SAMPLER,
						&srcSampler,
						nullptr,
						nullptr
					} );
				VkDescriptorImageInfo srcImageInfo
				{
					nullptr,
					srcView,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};
				get( result )->update( VkWriteDescriptorSet
					{
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						nullptr,
						result,
						0u,
						0u,
						1u,
						VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
						&srcImageInfo,
						nullptr,
						nullptr
					} );
				VkDescriptorImageInfo dstImageInfo
				{
					nullptr,
					dstView,
					VK_IMAGE_LAYOUT_GENERAL,
				};
				get( result )->update( VkWriteDescriptorSet
					{
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						nullptr,
						result,
						1u,
						0u,
						1u,
						VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
						&dstImageInfo,
						nullptr,
						nullptr
					} );
			}

			return result;
		}

			static VkImage doCreatePrivateImage( VkDevice device
				, VkImageCreateInfo createInfo
				, VkDeviceMemory & memory )
			{
				VkImage result{};
				auto status = allocate( result
					, get( device )->getAllocationCallbacks()
					, device
					, std::move( createInfo ) );
				if ( status != VK_SUCCESS || !result )
				{
					throw Exception{ status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status, "Blit temporary image allocation failed" };
				}
				auto requirements = get( result )->getMemoryRequirements();
				auto index = deduceMemoryType( requirements.memoryTypeBits
					, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
				status = allocate( memory
					, get( device )->getAllocationCallbacks()
					, device
					, VkMemoryAllocateInfo
					{
						VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
						nullptr,
						requirements.size,
						index,
					} );
				if ( status != VK_SUCCESS || !memory )
				{
					deallocate( result, get( device )->getAllocationCallbacks() );
					result = nullptr;
					memory = nullptr;
					throw Exception{ status == VK_SUCCESS ? VK_ERROR_OUT_OF_DEVICE_MEMORY : status, "Blit temporary image-memory allocation failed" };
				}
				status = get( result )->bindMemory( memory, 0u );
				if ( status != VK_SUCCESS )
				{
					deallocate( result, get( device )->getAllocationCallbacks() );
					result = nullptr;
					deallocate( memory, get( device )->getAllocationCallbacks() );
					memory = nullptr;
					throw Exception{ status, "Blit temporary image memory bind failed" };
				}

				return result;
			}

			static VkImage doGetImage( VkDevice device
				, VkImage image
				, VkDeviceMemory & memory
				, VkImageUsageFlags requiredUsage )
			{
				auto createInfo = get( image )->getCreateInfo();
				if ( checkFlag( createInfo.usage, requiredUsage ) ) return image;
				createInfo.usage |= requiredUsage;
				return doCreatePrivateImage( device, std::move( createInfo ), memory );
			}

		static VkImage doGetSamplable( VkDevice device
			, VkImage image
			, VkDeviceMemory & memory )
		{
			return doGetImage( device
				, image
				, memory
				, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT );
		}

			static VkImage doGetStorable( VkDevice device
				, VkImage image
				, VkDeviceMemory & memory )
		{
				return doGetImage( device
					, image
					, memory
					, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT );
			}

			static VkImage doCreateCompactStorable( VkDevice device
				, VkImage image
				, VkDeviceMemory & memory
				, blitscratch::Layout const & layout )
			{
				auto createInfo = get( image )->getCreateInfo();
				createInfo.extent = layout.scratchExtent;
				createInfo.mipLevels = 1u;
				createInfo.arrayLayers = layout.layerCount;
				createInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT
					| VK_IMAGE_USAGE_TRANSFER_DST_BIT
					| VK_IMAGE_USAGE_STORAGE_BIT;
				return doCreatePrivateImage( device, std::move( createInfo ), memory );
			}

			static bool getCompactPayloadBytes( VkFormat format
				, blitscratch::Layout const & layout
				, uint64_t & result )noexcept
			{
				// The compute path deliberately admits RGBA8_UNORM output only.  This
				// is texel payload accounting, never a claim about D3D allocation size.
				if ( format != VK_FORMAT_R8G8B8A8_UNORM
					|| !layout.scratchExtent.width || !layout.scratchExtent.height || !layout.layerCount )
				{
					return false;
				}
				auto const maximum = ( std::numeric_limits< uint64_t >::max )();
				auto bytes = uint64_t( layout.scratchExtent.width );
				if ( bytes > maximum / layout.scratchExtent.height ) return false;
				bytes *= layout.scratchExtent.height;
				if ( bytes > maximum / layout.layerCount ) return false;
				bytes *= layout.layerCount;
				if ( bytes > maximum / 4u ) return false;
				result = bytes * 4u;
				return true;
			}

			static bool areInSameGroup( DXGI_FORMAT src, DXGI_FORMAT dst )
		{
			return getDxgiFormatGroup( src ) == getDxgiFormatGroup( dst );
		}

		static bool areBlitCompatible( VkFormat src, VkFormat dst )
		{
			if ( src == dst
				|| areInSameGroup( getDxgiFormat( src ), getDxgiFormat( dst ) ) )
			{
				return true;
			}

			switch ( src )
			{
			case VK_FORMAT_R32_UINT:
			case VK_FORMAT_R32_SINT:
				return dst == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;

			case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
				return dst == VK_FORMAT_R32_UINT
					|| dst == VK_FORMAT_R32_SINT;

			case VK_FORMAT_R16G16B16A16_UINT:
			case VK_FORMAT_R16G16B16A16_SINT:
			case VK_FORMAT_R32G32_UINT:
			case VK_FORMAT_R32G32_SINT:
				return dst == VK_FORMAT_BC1_RGB_SRGB_BLOCK
					|| dst == VK_FORMAT_BC1_RGB_UNORM_BLOCK
					|| dst == VK_FORMAT_BC4_UNORM_BLOCK
					|| dst == VK_FORMAT_BC4_SNORM_BLOCK;

			case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
			case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
			case VK_FORMAT_BC4_UNORM_BLOCK:
			case VK_FORMAT_BC4_SNORM_BLOCK:
				return dst == VK_FORMAT_R16G16B16A16_UINT
					|| dst == VK_FORMAT_R16G16B16A16_SINT
					|| dst == VK_FORMAT_R32G32_UINT
					|| dst == VK_FORMAT_R32G32_SINT;

			case VK_FORMAT_R32G32B32A32_UINT:
			case VK_FORMAT_R32G32B32A32_SINT:
				return dst == VK_FORMAT_BC2_UNORM_BLOCK
					|| dst == VK_FORMAT_BC2_SRGB_BLOCK
					|| dst == VK_FORMAT_BC3_UNORM_BLOCK
					|| dst == VK_FORMAT_BC3_SRGB_BLOCK
					|| dst == VK_FORMAT_BC5_UNORM_BLOCK
					|| dst == VK_FORMAT_BC5_SNORM_BLOCK;

			case VK_FORMAT_BC2_SRGB_BLOCK:
			case VK_FORMAT_BC2_UNORM_BLOCK:
			case VK_FORMAT_BC3_SRGB_BLOCK:
			case VK_FORMAT_BC3_UNORM_BLOCK:
			case VK_FORMAT_BC5_UNORM_BLOCK:
			case VK_FORMAT_BC5_SNORM_BLOCK:
				return dst == VK_FORMAT_R32G32B32A32_UINT
					|| dst == VK_FORMAT_R32G32B32A32_SINT;

			default:
				return false;
			}
		}

		static VkImageCopy makeImageCopy( VkImageBlit const & region
			, int direction )
		{
			auto const sourceSide = direction != 2;
			auto const & sourceSubresource = sourceSide ? region.srcSubresource : region.dstSubresource;
			auto const & sourceOffset = sourceSide ? region.srcOffsets[0] : region.dstOffsets[0];
			auto const & sourceEnd = sourceSide ? region.srcOffsets[1] : region.dstOffsets[1];
			// Temporary images have the full original dimensions.  Their requested
			// region therefore retains its original local origin, not a fake zero.
			return VkImageCopy
			{
				sourceSubresource,
				sourceOffset,
				( direction == 0 ? region.dstSubresource : sourceSubresource ),
				( direction == 0 ? region.dstOffsets[0] : sourceOffset ),
				VkExtent3D
				{
					uint32_t( sourceEnd.x - sourceOffset.x ),
					uint32_t( sourceEnd.y - sourceOffset.y ),
					uint32_t( sourceEnd.z - sourceOffset.z ),
				},
			};
		}
	}
	
		//*********************************************************************************************

		BlitSamplerOwner::~BlitSamplerOwner()noexcept
		{
			if ( sampler && device ) deallocate( sampler, get( device )->getAllocationCallbacks() );
		}

		BlitPipeline::BlitPipeline( Device const & owner, VkFormat src, VkFormat dst )
		: device{ get( &owner ) }
	{
		try
		{
			owner.requireHealthy( "Blit pipeline construction" );
			descriptorLayout = blitimg::doCreateBlitDescriptorLayout( owner );
			pipelineLayout = blitimg::doCreateBlitPipelineLayout( owner, descriptorLayout );
			shader = blitimg::doCreateShaderModule( owner, src, dst, spirv );
			pipeline = blitimg::doCreateBlitPipeline( owner, pipelineLayout, shader );
		}
		catch ( ... )
		{
			deallocate( pipeline, owner.getAllocationCallbacks() );
			deallocate( shader, owner.getAllocationCallbacks() );
			deallocate( pipelineLayout, owner.getAllocationCallbacks() );
			deallocate( descriptorLayout, owner.getAllocationCallbacks() );
			throw;
		}
	}

	BlitPipeline::~BlitPipeline()noexcept
	{
		deallocate( pipeline, get( device )->getAllocationCallbacks() );
		deallocate( shader, get( device )->getAllocationCallbacks() );
		deallocate( pipelineLayout, get( device )->getAllocationCallbacks() );
		deallocate( descriptorLayout, get( device )->getAllocationCallbacks() );
	}

	//*********************************************************************************************

	BlitImageCommand::Attachment::Attachment( VkDevice device
		, VkImageSubresourceLayers const & subresource
		, VkImage image
		, uint32_t layer )
		: device{ device }
		, image{ get( image )->getResource() }
		, subResourceIndex{ get( image )->getNativeSubresource( subresource.mipLevel
			, subresource.baseArrayLayer + layer ) }
	{
			auto status = allocate( view
			, get( device )->getAllocationCallbacks()
			, device
			, VkImageViewCreateInfo
			{
				VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				nullptr,
				0u,
				image,
				get( image )->getType() == VK_IMAGE_TYPE_1D
					? VK_IMAGE_VIEW_TYPE_1D
					: get( image )->getType() == VK_IMAGE_TYPE_2D
						? VK_IMAGE_VIEW_TYPE_2D
						: VK_IMAGE_VIEW_TYPE_3D,
				get( image )->getFormat(),
				VkComponentMapping{},
				VkImageSubresourceRange{ getAspectMask( get( image )->getFormat() ), subresource.mipLevel, 1u, subresource.baseArrayLayer + layer, 1u }
			} );
			if ( status != VK_SUCCESS || !view )
			{
				throw Exception{ status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status, "Blit image-view allocation failed" };
			}
	}

	BlitImageCommand::Attachment::~Attachment()noexcept
	{
		deallocate( view, get( device )->getAllocationCallbacks() );
	}

	//*********************************************************************************************

	BlitImageCommand::LayerCopy::LayerCopy( VkDevice device
		, VkDescriptorPool pool
		, VkDescriptorSetLayout descriptorLayout
		, VkBuffer ubo
		, VkDeviceSize offset
		, VkDeviceSize range
		, VkSampler sampler
		, VkImageBlit const & blitRegion
		, VkImage srcImage
		, VkImage dstImage
		, uint32_t layer )
		: dstOffset{ blitRegion.dstOffsets[0] }
		, srcBox
		{
			UINT( blitRegion.srcOffsets[0].x ),
			UINT( blitRegion.srcOffsets[0].y ),
			UINT( blitRegion.srcOffsets[0].z ),
			UINT( blitRegion.srcOffsets[1].x ),
			UINT( blitRegion.srcOffsets[1].y ),
			UINT( blitRegion.srcOffsets[1].z )
		}
		, dstBox
		{
			UINT( blitRegion.dstOffsets[0].x ),
			UINT( blitRegion.dstOffsets[0].y ),
			UINT( blitRegion.dstOffsets[0].z ),
			UINT( blitRegion.dstOffsets[1].x ),
			UINT( blitRegion.dstOffsets[1].y ),
			UINT( blitRegion.dstOffsets[1].z )
		}
		, src{ device, blitRegion.srcSubresource, srcImage, layer }
		, dst{ device, blitRegion.dstSubresource, dstImage, layer }
			, set{ blitimg::doCreateDescriptorSet( device
			, pool
			, descriptorLayout
			, ubo
			, offset
			, range
			, sampler
			, src.view
			, dst.view ) }
		, blitBox
		{
			{
					float( blitRegion.srcOffsets[0].x ),
					float( blitRegion.srcOffsets[0].y ),
					float( blitRegion.srcOffsets[1].x - blitRegion.srcOffsets[0].x ),
					float( blitRegion.srcOffsets[1].y - blitRegion.srcOffsets[0].y ),
			},
			{
				float( blitRegion.dstOffsets[0].x ),
				float( blitRegion.dstOffsets[0].y ),
					float( blitRegion.dstOffsets[1].x - blitRegion.dstOffsets[0].x ),
					float( blitRegion.dstOffsets[1].y - blitRegion.dstOffsets[0].y ),
			},
			}
		{
			if ( !set ) throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Blit descriptor-set allocation failed" };
		}

	//*********************************************************************************************

	BlitImageCommand::BlitImageCommand( VkCommandPool pool
		, VkCommandBuffer cb
		, VkDevice device
		, BlitPipeline const & pipeline
		, VkImage srcImage
		, VkImage dstImage
		, ArrayView< VkImageBlit const > const & regions
		, VkFilter filter )
		: CommandBase{ device }
		, m_srcTexture{ srcImage }
		, m_dstTexture{ dstImage }
		, m_commandPool{ pool }
	{
		try
		{
		uint32_t count = 0u;
		if ( !srcImage || !dstImage )
		{
			throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Blit image received a null image" };
		}

		for ( auto & region : regions )
		{
			if ( region.srcSubresource.mipLevel >= get( srcImage )->getMipmapLevels()
                || region.dstSubresource.mipLevel >= get( dstImage )->getMipmapLevels()
                || region.srcSubresource.mipLevel >= 32u || region.dstSubresource.mipLevel >= 32u
                || uint64_t( region.srcSubresource.baseArrayLayer ) + region.srcSubresource.layerCount > get( srcImage )->getCreateInfo().arrayLayers
                || uint64_t( region.dstSubresource.baseArrayLayer ) + region.dstSubresource.layerCount > get( dstImage )->getCreateInfo().arrayLayers )
            { throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Blit mip/layer range exceeds image" }; }
            auto srcExtent = get( srcImage )->getDimensions();
            auto dstExtent = get( dstImage )->getDimensions();
            if ( region.srcOffsets[1].x > int32_t( std::max( 1u, srcExtent.width >> region.srcSubresource.mipLevel ) )
                || region.srcOffsets[1].y > int32_t( std::max( 1u, srcExtent.height >> region.srcSubresource.mipLevel ) )
                || region.dstOffsets[1].x > int32_t( std::max( 1u, dstExtent.width >> region.dstSubresource.mipLevel ) )
                || region.dstOffsets[1].y > int32_t( std::max( 1u, dstExtent.height >> region.dstSubresource.mipLevel ) ) )
            { throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Blit rectangle exceeds selected logical mip" }; }
            auto const srcWidth = region.srcOffsets[1].x - region.srcOffsets[0].x;
			auto const srcHeight = region.srcOffsets[1].y - region.srcOffsets[0].y;
			auto const dstWidth = region.dstOffsets[1].x - region.dstOffsets[0].x;
			auto const dstHeight = region.dstOffsets[1].y - region.dstOffsets[0].y;
			if ( get( srcImage )->getType() != VK_IMAGE_TYPE_2D
				|| get( dstImage )->getType() != VK_IMAGE_TYPE_2D
				|| region.srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT
				|| region.dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT
				|| !region.srcSubresource.layerCount
				|| region.srcSubresource.layerCount != region.dstSubresource.layerCount
				|| region.srcOffsets[0].x < 0 || region.srcOffsets[0].y < 0
				|| region.dstOffsets[0].x < 0 || region.dstOffsets[0].y < 0
				|| srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0
				|| region.srcOffsets[0].z != 0 || region.srcOffsets[1].z != 1
				|| region.dstOffsets[0].z != 0 || region.dstOffsets[1].z != 1 )
			{
				reportError( device, VK_ERROR_FEATURE_NOT_PRESENT, "Blit image", "Unsupported negative, flipped, empty, non-colour, or non-2D blit region" );
				throw Exception{ VK_ERROR_FEATURE_NOT_PRESENT, "Unsupported HD5500 blit region" };
			}
			if ( isHd5500BlitTraceEnabled() && count < 16u )
			{
				std::fprintf( stderr, "[RW-HD5500-BLIT] rect src=(%d,%d)->(%d,%d) dst=(%d,%d)->(%d,%d) mips=%u/%u layers=%u/%u+%u\n",
					region.srcOffsets[0].x, region.srcOffsets[0].y, region.srcOffsets[1].x, region.srcOffsets[1].y,
					region.dstOffsets[0].x, region.dstOffsets[0].y, region.dstOffsets[1].x, region.dstOffsets[1].y,
					region.srcSubresource.mipLevel, region.dstSubresource.mipLevel, region.srcSubresource.layerCount,
					region.srcSubresource.baseArrayLayer, region.dstSubresource.baseArrayLayer );
			}

			if ( srcImage != dstImage
				&& srcWidth == dstWidth && srcHeight == dstHeight
				&& blitimg::areBlitCompatible( get( srcImage )->getFormat(), get( dstImage )->getFormat() ) )
			{
				m_layerBlits.push_back( blitimg::makeImageCopy( region, 0u ) );
			}
			else
			{
				auto const srcFormat = get( srcImage )->getFormat();
				auto const dstFormat = get( dstImage )->getFormat();
				if ( get( srcImage )->getSamplesCount() != VK_SAMPLE_COUNT_1_BIT
					|| get( dstImage )->getSamplesCount() != VK_SAMPLE_COUNT_1_BIT )
				{
					reportError( device, VK_ERROR_FEATURE_NOT_PRESENT, "Blit image", "HD5500 compute blit does not support multisampled images" );
					throw Exception{ VK_ERROR_FEATURE_NOT_PRESENT, "Unsupported multisampled HD5500 compute blit" };
				}
				if ( dstFormat != VK_FORMAT_R8G8B8A8_UNORM
					|| ( srcFormat != VK_FORMAT_R8G8B8A8_UNORM
						&& srcFormat != VK_FORMAT_BC7_UNORM_BLOCK
						&& srcFormat != VK_FORMAT_BC7_SRGB_BLOCK ) )
				{
					reportError( device, VK_ERROR_FORMAT_NOT_SUPPORTED, "Blit image", "HD5500 compute blit supports RGBA8_UNORM or BC7 source to RGBA8_UNORM destination only" );
					throw Exception{ VK_ERROR_FORMAT_NOT_SUPPORTED, "Unsupported HD5500 compute blit format" };
				}
				m_stretchRegions.push_back( region );
				if ( count > ( std::numeric_limits< uint32_t >::max )() - region.srcSubresource.layerCount )
				{
					throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Blit layer-copy count overflows" };
				}
				count += region.srcSubresource.layerCount;
				m_layerBlitsToTmp.push_back( blitimg::makeImageCopy( region, 1u ) );
			}
		}

		if ( count )
		{
			m_samplerOwner = blitimg::doGetCachedSampler( pipeline, filter );
			m_sampler = m_samplerOwner->sampler;
		}
		doInitialiseStretches( device, pipeline.descriptorLayout, count );

		doCreateCommandBuffer( pool
			, pipeline.pipeline
			, pipeline.pipelineLayout );
		get( cb )->executeCommands( makeArrayView( const_cast< VkCommandBuffer const * >( &m_commandBuffer ), 1u ) );
		}
		catch ( ... ) { doCleanup(); throw; }
	}

	void BlitImageCommand::doInitialiseStretchUbo( VkDevice device
		, VkDescriptorSetLayout descriptorLayout
		, uint32_t count )
	{
		VkDeviceSize range = 0u;
		m_ubo = blitimg::doCreateUbo( getDevice(), count, m_uboMemory, range );

		ashes::VkDescriptorPoolSizeArray poolSizes
		{
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count },
			{ VK_DESCRIPTOR_TYPE_SAMPLER, count * 3u },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count },
		};

		auto poolResult = allocate( m_pool
			, get( getDevice() )->getAllocationCallbacks()
			, getDevice()
			, VkDescriptorPoolCreateInfo
			{
				VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				nullptr,
				VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
				count,
				uint32_t( poolSizes.size() ),
				poolSizes.data()
			} );
		if ( poolResult != VK_SUCCESS || !m_pool )
		{
			throw Exception{ poolResult == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : poolResult, "Blit descriptor-pool allocation failed" };
		}

		VkDeviceSize offset = 0u;
		ashes::ByteArray data;
		data.resize( range * count );
		auto buffer = data.data();

		auto const & descriptorRegions = m_scratchStretchRegions.empty()
			? m_stretchRegions
			: m_scratchStretchRegions;
		for ( auto const & region : descriptorRegions )
		{
			for ( uint32_t layer = 0u; layer < region.srcSubresource.layerCount; ++layer )
			{
				m_layerStretches.emplace_back( std::make_shared< BlitImageCommand::LayerCopy >( getDevice()
					, m_pool
					, descriptorLayout
					, m_ubo
					, offset
					, range
					, m_sampler
					, region
					, m_tmpSrcTexture
					, m_tmpDstTexture
					, layer ) );
				if ( isHd5500BlitTraceEnabled() && m_layerStretches.size() <= 16u )
				{
					auto const & copy = *m_layerStretches.back();
					std::fprintf( stderr, "[RW-HD5500-BLIT] descriptor=%p srcSubresource=%u dstSubresource=%u dst=(%d,%d) extent=%gx%g\n",
						static_cast< void * >( copy.set ), copy.src.subResourceIndex, copy.dst.subResourceIndex,
						region.dstOffsets[0].x, region.dstOffsets[0].y, copy.blitBox.dstBox[2], copy.blitBox.dstBox[3] );
				}

				*reinterpret_cast< LayerCopy::BlitBox * >( buffer ) = m_layerStretches.back()->blitBox;
				offset += range;
				buffer += range;
			}
		}

		if ( offset != data.size() )
		{
			throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Blit UBO accounting failed" };
		}
		uint8_t * gpu = nullptr;
		auto lockResult = get( m_uboMemory )->lock( 0u, offset, 0u, reinterpret_cast< void ** >( &gpu ) );
		if ( lockResult != VK_SUCCESS ) throw Exception{ lockResult, "Blit UBO map failed" };

		if ( !gpu ) throw Exception{ VK_ERROR_MEMORY_MAP_FAILED, "Blit UBO map failed" };
		std::memcpy( gpu, data.data(), offset );
		auto context = get( device )->getImmediateContext();
		auto flushResult = get( m_uboMemory )->flush( context, 0u, offset );
		get( m_uboMemory )->unlock( context );
		if ( flushResult != VK_SUCCESS ) throw Exception{ flushResult, "Blit UBO flush failed" };
	}

	void BlitImageCommand::doInitialiseStretches( VkDevice device
		, VkDescriptorSetLayout descriptorLayout
		, uint32_t count )
	{
		if ( count )
		{
			m_tmpSrcTexture = blitimg::doGetSamplable( getDevice(), m_srcTexture, m_srcMemory );
			if ( m_srcTexture == m_dstTexture && m_tmpSrcTexture == m_srcTexture )
			{
				reportError( getDevice(), VK_ERROR_FEATURE_NOT_PRESENT, "Blit image", "In-place blit requires a private sampled source temporary" );
				throw Exception{ VK_ERROR_FEATURE_NOT_PRESENT, "Unsafe in-place HD5500 blit" };
			}

			if ( get( m_dstTexture )->isStorage() )
			{
				// Preserve the direct-storage destination path exactly: it neither
				// allocates nor accounts for a private destination image.
				m_tmpDstTexture = m_dstTexture;
			}
			else
			{
				auto const & dstInfo = get( m_dstTexture )->getCreateInfo();
				auto const layout = blitscratch::Layout::plan( dstInfo, m_stretchRegions );
				if ( layout.kind == blitscratch::LayoutKind::eInvalid )
				{
					reportError( getDevice(), VK_ERROR_INITIALIZATION_FAILED, "Blit image", layout.reason );
					throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Invalid compact HD5500 blit layout" };
				}

				if ( layout.kind == blitscratch::LayoutKind::eCompact )
				{
					uint64_t payloadBytes{};
					if ( !blitimg::getCompactPayloadBytes( dstInfo.format, layout, payloadBytes ) )
					{
						reportError( getDevice(), VK_ERROR_INITIALIZATION_FAILED, "Blit image", "Compact blit scratch payload is unrepresentable" );
						throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Unrepresentable compact HD5500 blit scratch" };
					}

					try
					{
						m_scratchStretchRegions.reserve( m_stretchRegions.size() );
						m_layerBlitsFromTmp.reserve( m_stretchRegions.size() );
						for ( auto const & region : m_stretchRegions )
						{
							m_scratchStretchRegions.push_back( layout.makeLocalDestination( region ) );
							m_layerBlitsFromTmp.push_back( layout.makeCopyBack( region ) );
						}

						m_tmpDstTexture = blitimg::doCreateCompactStorable( getDevice(), m_dstTexture, m_dstMemory, layout );
						m_compactScratch.atlasExtent = layout.atlasExtent;
						m_compactScratch.scratchExtent = layout.scratchExtent;
						m_compactScratch.bboxOrigin = layout.bboxOrigin;
						m_compactScratch.mipLevel = layout.mipLevel;
						m_compactScratch.baseArrayLayer = layout.baseArrayLayer;
						m_compactScratch.layerCount = layout.layerCount;
						m_compactScratch.payloadBytes = payloadBytes;
						m_compactScratch.id = recordCompactScratchAllocation( layout.atlasExtent, layout.scratchExtent
							, layout.bboxOrigin, layout.mipLevel, layout.baseArrayLayer, layout.layerCount, payloadBytes );
						m_compactScratch.allocated = true;
					}
					catch ( ... )
					{
						recordScratchFallbackOrFailure( "alloc-failed", layout.atlasExtent, layout.scratchExtent, layout.bboxOrigin
							, layout.mipLevel, layout.baseArrayLayer, layout.layerCount, "compact", layout.reason, "allocation-or-setup-failed" );
						throw;
					}
				}
				else
				{
					// Valid but intentionally unsupported compact shape.  The established
					// full clone retains native mip/array semantics and reports why it was used.
					recordScratchFallbackOrFailure( "fallback", layout.atlasExtent, layout.scratchExtent, layout.bboxOrigin
						, ( std::numeric_limits< uint32_t >::max )(), 0u, dstInfo.arrayLayers, "full", layout.reason );
					try
					{
						m_layerBlitsFromTmp.reserve( m_stretchRegions.size() );
						for ( auto const & region : m_stretchRegions )
						{
							m_layerBlitsFromTmp.push_back( blitimg::makeImageCopy( region, 2u ) );
						}
						m_tmpDstTexture = blitimg::doGetStorable( getDevice(), m_dstTexture, m_dstMemory );
					}
					catch ( ... )
					{
						recordScratchFallbackOrFailure( "alloc-failed", layout.atlasExtent, layout.scratchExtent, layout.bboxOrigin
							, ( std::numeric_limits< uint32_t >::max )(), 0u, dstInfo.arrayLayers, "full", layout.reason, "allocation-or-setup-failed" );
						throw;
					}
				}
			}
			doInitialiseStretchUbo( device, descriptorLayout, count );
		}
	}

	VkImage BlitImageCommand::doUpdateTmpSrc()
	{
		auto srcTexture = m_srcTexture;

		if ( m_tmpSrcTexture != nullptr
			&& m_tmpSrcTexture != m_srcTexture
			&& !m_layerBlitsToTmp.empty() )
		{
			// Copy source to staging
			get( m_commandBuffer )->copyImage( m_srcTexture
				, m_tmpSrcTexture
				, makeArrayView( const_cast< VkImageCopy const * >( m_layerBlitsToTmp.data() )
					, m_layerBlitsToTmp.size() ) );
			srcTexture = m_tmpSrcTexture;
		}

		return srcTexture;
	}

	void BlitImageCommand::doUpdateTmpDst()
	{
		if ( m_tmpDstTexture != nullptr
			&& m_tmpDstTexture != m_dstTexture
			&& !m_layerBlitsFromTmp.empty() )
		{
			// Copy staging to destination
			get( m_commandBuffer )->copyImage( m_tmpDstTexture
				, m_dstTexture
				, makeArrayView( const_cast< VkImageCopy const * >( m_layerBlitsFromTmp.data() )
					, m_layerBlitsFromTmp.size() ) );
		}
	}

	void BlitImageCommand::doAddBlits( [[maybe_unused]] VkImage srcTexture )
	{
		if ( !m_layerBlits.empty() )
		{
			get( m_commandBuffer )->copyImage( m_srcTexture
				, m_dstTexture
				, makeArrayView( const_cast< VkImageCopy const * >( m_layerBlits.data() )
					, m_layerBlits.size() ) );
		}
	}

	void BlitImageCommand::doAddStretches( VkPipeline pipeline
		, VkPipelineLayout pipelineLayout )
	{
		if ( !m_layerStretches.empty() )
		{
			get( m_commandBuffer )->bindPipeline( pipeline, VK_PIPELINE_BIND_POINT_COMPUTE );

			for ( auto const & copy : m_layerStretches )
			{
			uint32_t dstWidth = copy->dstBox.right - copy->dstBox.left;
			auto dstHeight = uint32_t( copy->dstBox.bottom - copy->dstBox.top );
				get( m_commandBuffer )->bindDescriptorSets( VK_PIPELINE_BIND_POINT_COMPUTE
					, pipelineLayout
					, 0u
					, makeArrayView( const_cast< VkDescriptorSet const * >( &copy->set ), 1u )
					, {} );
			if ( dstWidth && dstHeight )
			{
				get( m_commandBuffer )->dispatch( ( dstWidth + 7u ) / 8u, ( dstHeight + 7u ) / 8u, 1u );
			}
			}
		}
	}

	void BlitImageCommand::doCreateCommandBuffer( VkCommandPool pool
		, VkPipeline pipeline
		, VkPipelineLayout pipelineLayout )
	{
		auto allocationResult = allocateNA( m_commandBuffer
			, getDevice()
			, pool
			, true );
		if ( allocationResult != VK_SUCCESS || !m_commandBuffer ) throw Exception{ allocationResult == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : allocationResult, "Blit child command-buffer allocation failed" };
		get( pool )->detachCommands( m_commandBuffer );
		auto result = get( m_commandBuffer )->begin( VkCommandBufferBeginInfo
			{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				nullptr,
				VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
				nullptr
			} );
		if ( result != VK_SUCCESS ) throw Exception{ result, "Blit child command-buffer begin failed" };
		VkImage image = doUpdateTmpSrc();
		doAddBlits( image );
		doAddStretches( pipeline
			, pipelineLayout );
		doUpdateTmpDst();
		result = get( m_commandBuffer )->end();
		if ( result != VK_SUCCESS ) throw Exception{ result, "Blit child command-buffer end failed" };
	}

	BlitImageCommand::~BlitImageCommand()noexcept
	{
		if ( m_lifetime && m_lifetime.use_count() == 1u ) doCleanup();
	}

	void BlitImageCommand::doCleanup()noexcept
	{
		// Disconnect child memory callbacks before its referenced views/resources.
		deallocateNA( m_commandBuffer );
		m_commandBuffer = nullptr;
		// LayerCopy owns the views and descriptors refer to those views.
		m_layerStretches.clear();
		deallocate( m_pool, get( getDevice() )->getAllocationCallbacks() );
		deallocate( m_ubo, get( getDevice() )->getAllocationCallbacks() );
		deallocate( m_uboMemory, get( getDevice() )->getAllocationCallbacks() );

		if ( m_tmpSrcTexture != m_srcTexture )
		{
			deallocate( m_tmpSrcTexture, get( getDevice() )->getAllocationCallbacks() );
			deallocate( m_srcMemory, get( getDevice() )->getAllocationCallbacks() );
		}

		if ( m_tmpDstTexture != m_dstTexture )
		{
			deallocate( m_tmpDstTexture, get( getDevice() )->getAllocationCallbacks() );
			deallocate( m_dstMemory, get( getDevice() )->getAllocationCallbacks() );
		}

		if ( m_compactScratch.allocated )
		{
			recordCompactScratchRelease( m_compactScratch.id, m_compactScratch.atlasExtent
				, m_compactScratch.scratchExtent, m_compactScratch.bboxOrigin
				, m_compactScratch.mipLevel, m_compactScratch.baseArrayLayer
				, m_compactScratch.layerCount, m_compactScratch.payloadBytes );
			m_compactScratch.allocated = false;
		}

		// The owner outlives flattened/cloned commands and invokes the same
		// allocator callback only when no command still references its sampler.
		m_samplerOwner.reset();

		m_pool = nullptr; m_ubo = nullptr; m_uboMemory = nullptr; m_sampler = nullptr;
		m_tmpSrcTexture = nullptr; m_tmpDstTexture = nullptr; m_srcMemory = nullptr; m_dstMemory = nullptr;
	}

	void BlitImageCommand::apply( Context & context )const
	{
	}

	CommandPtr BlitImageCommand::clone()const
	{
		return std::make_unique< BlitImageCommand >( *this );
	}

	//*********************************************************************************************
}
