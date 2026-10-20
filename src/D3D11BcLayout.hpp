#pragma once

#include <cstdint>
#include <cstdlib>

// BC resources require a block-aligned native base. A hidden mip prefix gives
// an exact logical-sized mip/view, without changing Vulkan format or contents.
//
// v12: the hidden prefix multiplies the native base by up to 4 per axis. When
// that expansion would exceed the D3D11 Texture2D limit (16384), a decodable
// 2D BC image is instead stored as an RGBA8 texture of exactly the logical
// size ("decoded" representation). The Vulkan-visible format, extent, mip
// count, memory requirements and mapped layout stay the BC ones; only the
// native backing differs. The decision is a pure function of
// VkImageCreateInfo, so image creation, views, copies and capability queries
// all agree on it.
namespace ashes::D3D11_NAMESPACE::bc
{
    enum class Representation : uint32_t
    {
        NotBc = 0u,
        NativeBc = 1u,
        DecodedRgba8 = 2u,
        Unsupported = 3u,
    };

    inline constexpr uint32_t MaxTexture2DAxis = 16384u;

    inline bool isFormat(VkFormat format) noexcept
    {
        return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
    }

    inline bool isSrgb(VkFormat format) noexcept
    {
        switch (format)
        {
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return true;
        default:
            return false;
        }
    }

    // Formats with an exact or conventional 8-bit UNORM decode. Signed BC4/BC5
    // and HDR BC6H are deliberately excluded: an RGBA8 UNORM backing cannot
    // represent their sampled values.
    inline bool isDecodable(VkFormat format) noexcept
    {
        switch (format)
        {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return true;
        default:
            return false;
        }
    }

    // RW_HD5500_BC_DECODE_FALLBACK=0 restores the v11 behaviour (oversized odd
    // BC images fail with VK_ERROR_FORMAT_NOT_SUPPORTED).
    inline bool decodeFallbackEnabled() noexcept
    {
        static bool const enabled = []()
        {
            char value[4]{};
            auto const length = GetEnvironmentVariableA("RW_HD5500_BC_DECODE_FALLBACK", value, 4u);
            return !(length == 1u && value[0] == '0');
        }();
        return enabled;
    }

    // The v1-v11 hidden-prefix rule, independent of representation.
    inline UINT rawPrefix(VkImageCreateInfo const &info) noexcept
    {
        if (!isFormat(info.format) || info.imageType != VK_IMAGE_TYPE_2D) return 0u;
        UINT result{};
        while (result < 2u && (((info.extent.width << result) & 3u) || ((info.extent.height << result) & 3u))) ++result;
        return result;
    }

    inline bool prefixFits(VkImageCreateInfo const &info) noexcept
    {
        auto const p = rawPrefix(info);
        return (uint64_t(info.extent.width) << p) <= MaxTexture2DAxis
            && (uint64_t(info.extent.height) << p) <= MaxTexture2DAxis;
    }

    // Usage bits that a decoded RGBA8 backing reproduces exactly: sampling and
    // transfers that never need the original block bytes back.
    inline bool decodedUsageSupported(VkImageUsageFlags usage) noexcept
    {
        VkImageUsageFlags const allowed = VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_DST_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        return (usage & ~allowed) == 0u;
    }

    inline Representation representation(VkImageCreateInfo const &info) noexcept
    {
        if (!isFormat(info.format)) return Representation::NotBc;
        if (info.imageType != VK_IMAGE_TYPE_2D || prefixFits(info)) return Representation::NativeBc;
        if (decodeFallbackEnabled()
            && isDecodable(info.format)
            && info.extent.width <= MaxTexture2DAxis
            && info.extent.height <= MaxTexture2DAxis
            && info.samples == VK_SAMPLE_COUNT_1_BIT
            && info.tiling == VK_IMAGE_TILING_OPTIMAL
            && decodedUsageSupported(info.usage))
        {
            return Representation::DecodedRgba8;
        }
        return Representation::Unsupported;
    }

    inline bool isDecoded(VkImageCreateInfo const &info) noexcept
    {
        return representation(info) == Representation::DecodedRgba8;
    }

    inline char const *representationName(Representation value) noexcept
    {
        switch (value)
        {
        case Representation::NotBc: return "not-bc";
        case Representation::NativeBc: return "native-bc";
        case Representation::DecodedRgba8: return "decoded-rgba8";
        default: return "unsupported";
        }
    }

    inline VkFormat decodedFormat(VkFormat format) noexcept
    {
        return isSrgb(format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }

    // Vulkan format whose DXGI mapping is used for the native texture.
    inline VkFormat nativeFormat(VkImageCreateInfo const &info) noexcept
    {
        return isDecoded(info) ? decodedFormat(info.format) : info.format;
    }

    // Vulkan format whose DXGI mapping is used for a native view of the image.
    inline VkFormat nativeViewFormat(VkImageCreateInfo const &info, VkFormat viewFormat) noexcept
    {
        return isDecoded(info) && isFormat(viewFormat) ? decodedFormat(viewFormat) : viewFormat;
    }

    inline UINT prefix(VkImageCreateInfo const &info) noexcept
    {
        return isDecoded(info) ? 0u : rawPrefix(info);
    }

    inline UINT mipCount(VkImageCreateInfo const &info) noexcept
    {
        return info.mipLevels + prefix(info);
    }

    inline UINT subresource(VkImageCreateInfo const &info, UINT level, UINT layer) noexcept
    {
        return D3D11CalcSubresource(level + prefix(info), layer, mipCount(info));
    }

    inline UINT blockBytes(VkFormat format) noexcept
    {
        return (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK)
            || format == VK_FORMAT_BC4_UNORM_BLOCK || format == VK_FORMAT_BC4_SNORM_BLOCK ? 8u : 16u;
    }

    inline UINT rowBytes(VkFormat format, UINT width) noexcept
    {
        return ((width + 3u) / 4u) * blockBytes(format);
    }

    inline UINT rows(UINT height) noexcept
    {
        return (height + 3u) / 4u;
    }

    // Bytes of a full mip chain for a given layout; used for diagnostics only.
    inline uint64_t chainBytes(VkImageCreateInfo const &info, bool decoded) noexcept
    {
        uint64_t total{};
        for (uint32_t level = 0u; level < info.mipLevels; ++level)
        {
            uint32_t const w = info.extent.width >> level ? info.extent.width >> level : 1u;
            uint32_t const h = info.extent.height >> level ? info.extent.height >> level : 1u;
            total += decoded ? uint64_t(w) * h * 4u : uint64_t(rowBytes(info.format, w)) * rows(h);
        }
        return total * (info.arrayLayers ? info.arrayLayers : 1u);
    }
}
