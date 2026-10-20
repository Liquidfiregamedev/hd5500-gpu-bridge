#pragma once
#include <vulkan/vulkan.h>

namespace ashes::D3D11_NAMESPACE::bccopy
{
    // Whole-subresource selection is permitted only when both real native mip
    // extents exactly match the requested copy. Never enlarge a partial region.
    inline bool wholeSubresource(VkFormat format, VkImageCopy const &copy,
        VkExtent3D const &src, VkExtent3D const &dst) noexcept
    {
        return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK
            && format <= VK_FORMAT_BC7_SRGB_BLOCK
            && copy.srcOffset.x == 0 && copy.srcOffset.y == 0 && copy.srcOffset.z == 0
            && copy.dstOffset.x == 0 && copy.dstOffset.y == 0 && copy.dstOffset.z == 0
            && copy.extent.width == src.width && copy.extent.height == src.height
            && copy.extent.depth == src.depth
            && copy.extent.width == dst.width && copy.extent.height == dst.height
            && copy.extent.depth == dst.depth;
    }
}
