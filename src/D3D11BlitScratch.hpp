/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace ashes::D3D11_NAMESPACE::blitscratch
{
	// This small value type is intentionally independent of command recording so
	// rectangle/layer planning can be unit-tested without a D3D11 device.
	enum class LayoutKind
	{
		eCompact,
		eFullFallback,
		eInvalid,
	};

	struct Layout
	{
		LayoutKind kind{ LayoutKind::eInvalid };
		char const * reason{ "empty-stretch-set" };
		VkExtent3D atlasExtent{};
		VkExtent3D scratchExtent{};
		VkOffset3D bboxOrigin{};
		uint32_t mipLevel{};
		uint32_t baseArrayLayer{};
		uint32_t layerCount{};

		static Layout plan( VkImageCreateInfo const & destination
			, ArrayView< VkImageBlit const > const & regions )noexcept
		{
			if ( destination.imageType != VK_IMAGE_TYPE_2D || destination.extent.depth != 1u )
			{
				Layout result{};
				result.reason = "destination-not-2d";
				return result;
			}
			return plan( destination.extent, destination.mipLevels, destination.arrayLayers, destination.flags
				, regions.empty() ? nullptr : regions.data(), regions.size() );
		}

		static Layout plan( VkImageCreateInfo const & destination
			, std::vector< VkImageBlit > const & regions )noexcept
		{
			if ( destination.imageType != VK_IMAGE_TYPE_2D || destination.extent.depth != 1u )
			{
				Layout result{};
				result.reason = "destination-not-2d";
				return result;
			}
			return plan( destination.extent, destination.mipLevels, destination.arrayLayers, destination.flags
				, regions.data(), regions.size() );
		}

		static Layout plan( VkExtent3D const & atlasExtent
			, uint32_t atlasMipLevels
			, uint32_t atlasLayers
			, VkImageCreateFlags imageFlags
			, VkImageBlit const * regions
			, size_t count ) noexcept
		{
			Layout result{};
			result.atlasExtent = atlasExtent;
			result.scratchExtent = atlasExtent;
			result.layerCount = atlasLayers;

			if ( !atlasExtent.width || !atlasExtent.height || !atlasMipLevels || !atlasLayers )
			{
				result.reason = "invalid-destination-image-bounds";
				return result;
			}

			if ( !regions || !count )
			{
				return result;
			}

			uint32_t const commonMip = regions[0].dstSubresource.mipLevel;
			int32_t minX = ( std::numeric_limits< int32_t >::max )();
			int32_t minY = ( std::numeric_limits< int32_t >::max )();
			int32_t maxX = ( std::numeric_limits< int32_t >::min )();
			int32_t maxY = ( std::numeric_limits< int32_t >::min )();
			uint32_t minLayer = ( std::numeric_limits< uint32_t >::max )();
			uint64_t maxLayer = 0u;
			bool sameMip = true;

			for ( size_t index = 0u; index < count; ++index )
			{
				auto const & region = regions[index];
				auto const & subresource = region.dstSubresource;
				if ( subresource.mipLevel >= atlasMipLevels || subresource.mipLevel >= 32u
					|| subresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT
					|| !subresource.layerCount
					|| region.dstOffsets[0].x < 0 || region.dstOffsets[0].y < 0
					|| region.dstOffsets[0].z != 0 || region.dstOffsets[1].z != 1
					|| region.dstOffsets[1].x <= region.dstOffsets[0].x
					|| region.dstOffsets[1].y <= region.dstOffsets[0].y )
				{
					result.reason = "invalid-destination-region";
					return result;
				}
				sameMip = sameMip && subresource.mipLevel == commonMip;

				auto const layerEnd = uint64_t( subresource.baseArrayLayer ) + subresource.layerCount;
				if ( !atlasLayers || layerEnd > atlasLayers )
				{
					result.reason = "destination-layer-range";
					return result;
				}

				auto const mipWidth = atlasExtent.width >> subresource.mipLevel;
				auto const mipHeight = atlasExtent.height >> subresource.mipLevel;
				auto const width = mipWidth ? mipWidth : 1u;
				auto const height = mipHeight ? mipHeight : 1u;
				if ( region.dstOffsets[1].x > int32_t( width )
					|| region.dstOffsets[1].y > int32_t( height ) )
				{
					result.reason = "destination-rectangle-out-of-mip";
					return result;
				}

				minX = ( std::min )( minX, region.dstOffsets[0].x );
				minY = ( std::min )( minY, region.dstOffsets[0].y );
				maxX = ( std::max )( maxX, region.dstOffsets[1].x );
				maxY = ( std::max )( maxY, region.dstOffsets[1].y );
				minLayer = ( std::min )( minLayer, subresource.baseArrayLayer );
				maxLayer = ( std::max )( maxLayer, layerEnd );
			}

			// A compact texture has one mip.  A batch that names several destination
			// mips is valid, but it must retain the established full-clone path.
			if ( !sameMip )
			{
				result.kind = LayoutKind::eFullFallback;
				result.reason = "multiple-destination-mips";
				return result;
			}

			if ( imageFlags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT )
			{
				result.kind = LayoutKind::eFullFallback;
				result.reason = "cube-compatible-image";
				return result;
			}

			if ( maxX <= minX || maxY <= minY || maxLayer <= minLayer
				|| maxLayer - minLayer > ( std::numeric_limits< uint32_t >::max )() )
			{
				result.reason = "unrepresentable-compact-bounds";
				return result;
			}

			result.kind = LayoutKind::eCompact;
			result.reason = "common-mip-bbox";
			result.scratchExtent = VkExtent3D
			{
				uint32_t( maxX - minX ),
				uint32_t( maxY - minY ),
				1u,
			};
			result.bboxOrigin = VkOffset3D{ minX, minY, 0 };
			result.mipLevel = commonMip;
			result.baseArrayLayer = minLayer;
			result.layerCount = uint32_t( maxLayer - minLayer );
			return result;
		}

		VkImageBlit makeLocalDestination( VkImageBlit const & original )const noexcept
		{
			VkImageBlit result = original;
			result.dstSubresource.mipLevel = 0u;
			result.dstSubresource.baseArrayLayer -= baseArrayLayer;
			result.dstOffsets[0].x -= bboxOrigin.x;
			result.dstOffsets[0].y -= bboxOrigin.y;
			result.dstOffsets[1].x -= bboxOrigin.x;
			result.dstOffsets[1].y -= bboxOrigin.y;
			return result;
		}

		VkImageCopy makeCopyBack( VkImageBlit const & original )const noexcept
		{
			auto const local = makeLocalDestination( original );
			return VkImageCopy
			{
				local.dstSubresource,
				local.dstOffsets[0],
				original.dstSubresource,
				original.dstOffsets[0],
				VkExtent3D
				{
					uint32_t( original.dstOffsets[1].x - original.dstOffsets[0].x ),
					uint32_t( original.dstOffsets[1].y - original.dstOffsets[0].y ),
					1u,
				},
			};
		}
	};
}
