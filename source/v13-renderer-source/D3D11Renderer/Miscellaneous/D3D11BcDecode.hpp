#pragma once

// CPU decode of BC block rows into RGBA8 texels for the v12 decoded
// representation (see D3D11BcLayout.hpp). Uses the vendored bcdec v0.985
// (iOrange/bcdec, MIT or Unlicense; see licenses/bcdec-LICENSE.txt).
#include "Miscellaneous/D3D11BcLayout.hpp"

#include <cstdint>
#include <cstring>

#ifndef BCDEC_IMPLEMENTATION
#	define BCDEC_STATIC
#	define BCDEC_IMPLEMENTATION
#	include "Miscellaneous/bcdec.h"
#	undef BCDEC_IMPLEMENTATION
#endif

namespace ashes::D3D11_NAMESPACE::bcdecode
{
    // Decodes one 4x4 block into the RGBA8 destination (pitch in bytes).
    inline bool block(VkFormat format, uint8_t const *src, uint8_t *dst, int pitch) noexcept
    {
        switch (format)
        {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            bcdec_bc1(src, dst, pitch);
            // Vulkan BC1 RGB formats have no alpha: alpha always reads as 1.
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) dst[y * pitch + x * 4 + 3] = 0xFFu;
            return true;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            bcdec_bc1(src, dst, pitch);
            return true;
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
            bcdec_bc2(src, dst, pitch);
            return true;
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
            bcdec_bc3(src, dst, pitch);
            return true;
        case VK_FORMAT_BC4_UNORM_BLOCK:
        {
            uint8_t r[16];
            bcdec_bc4(src, r, 4);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                {
                    auto *p = dst + y * pitch + x * 4;
                    p[0] = r[y * 4 + x]; p[1] = 0u; p[2] = 0u; p[3] = 0xFFu;
                }
            return true;
        }
        case VK_FORMAT_BC5_UNORM_BLOCK:
        {
            uint8_t rg[32];
            bcdec_bc5(src, rg, 8);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                {
                    auto *p = dst + y * pitch + x * 4;
                    p[0] = rg[y * 8 + x * 2]; p[1] = rg[y * 8 + x * 2 + 1]; p[2] = 0u; p[3] = 0xFFu;
                }
            return true;
        }
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            bcdec_bc7(src, dst, pitch);
            return true;
        default:
            return false;
        }
    }

    // Decodes `blockRows` rows of `blocksWide` blocks. `src` points at the first
    // block of the first row, `srcRowPitch` is the byte distance between block
    // rows. The destination receives 4*blockRows texel rows of 4*blocksWide
    // RGBA8 texels with `dstPitch` bytes per texel row.
    inline bool rows(VkFormat format, uint8_t const *src, uint64_t srcRowPitch,
        uint32_t blocksWide, uint32_t blockRows, uint8_t *dst, uint32_t dstPitch) noexcept
    {
        auto const bytes = bc::blockBytes(format);
        for (uint32_t by = 0u; by < blockRows; ++by)
        {
            auto const *rowSrc = src + by * srcRowPitch;
            auto *rowDst = dst + uint64_t(by) * 4u * dstPitch;
            for (uint32_t bx = 0u; bx < blocksWide; ++bx)
            {
                if (!block(format, rowSrc + uint64_t(bx) * bytes, rowDst + uint64_t(bx) * 16u, int(dstPitch))) return false;
            }
        }
        return true;
    }

    // UpdateSubresource with a destination box on a deferred context applies
    // the box offset to the source pointer when the driver lacks native
    // command lists (documented D3D11 behaviour). Commands normally execute on
    // the immediate context; this keeps the decoded upload correct either way.
    inline void updateBox(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Resource *resource,
        UINT subresource, D3D11_BOX const &box, uint8_t const *data, UINT rowPitch, UINT depthPitch) noexcept
    {
        auto const *adjusted = data;
        if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED)
        {
            D3D11_FEATURE_DATA_THREADING threading{};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof(threading)))
                && !threading.DriverCommandLists)
            {
                adjusted = data - (uint64_t(box.front) * depthPitch) - (uint64_t(box.top) * rowPitch) - (uint64_t(box.left) * 4u);
            }
        }
        context->UpdateSubresource(resource, subresource, &box, adjusted, rowPitch, depthPitch);
    }
}
