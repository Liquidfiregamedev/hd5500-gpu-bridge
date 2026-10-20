#pragma once

// Included after ashesd3d11_api.hpp by D3D11DeviceMemory.cpp.
// Real D3D11 texture transfers; no Vulkan results or GPU properties are changed.
namespace ashes::D3D11_NAMESPACE
{
    namespace shadowtex
    {
        struct Range
        {
            ID3D11Texture2D *texture{};
            D3D11_TEXTURE2D_DESC desc{};
            VkDeviceSize offset{};
            UINT rowBytes{};
            UINT rows{};
            UINT sliceBytes{};
            UINT nativeSubresource{};
            bool touched{};
        };

        inline bool prepare(ObjectMemory const &memory, UINT subresource,
            VkDeviceSize requestOffset, VkDeviceSize requestSize, Range &range)
        {
            if (!memory.resource) return false;
            D3D11_RESOURCE_DIMENSION dimension{};
            memory.resource->GetType(&dimension);
            if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
            range.texture = static_cast<ID3D11Texture2D *>(memory.resource);
            range.texture->GetDesc(&range.desc);
            if (subresource >= memory.subresources.size())
            {
                checkError(memory.device, E_INVALIDARG, "TextureShadowSubresource");
                return true;
            }
            if (range.desc.MipLevels <= memory.mipPrefix)
            {
                checkError(memory.device, E_INVALIDARG, "TextureShadowNativeMipCount");
                return true;
            }
            auto logicalMips = range.desc.MipLevels - memory.mipPrefix;
            range.nativeSubresource = D3D11CalcSubresource(subresource % logicalMips + memory.mipPrefix,
                subresource / logicalMips, range.desc.MipLevels);
            range.offset = memory.offset;
            for (UINT i = 0; i < subresource; ++i)
            {
                if (range.offset > memory.allocateInfo.allocationSize ||
                    memory.subresources[i].SysMemSlicePitch > memory.allocateInfo.allocationSize - range.offset)
                {
                    checkError(memory.device, E_INVALIDARG, "TextureShadowOffset");
                    return true;
                }
                range.offset += memory.subresources[i].SysMemSlicePitch;
            }
            auto const &layout = memory.subresources[subresource];
            range.rowBytes = layout.SysMemPitch;
            range.sliceBytes = layout.SysMemSlicePitch;
            if (!range.rowBytes || !range.sliceBytes ||
                range.offset > memory.allocateInfo.allocationSize ||
                range.sliceBytes > memory.allocateInfo.allocationSize - range.offset)
            {
                checkError(memory.device, E_INVALIDARG, "TextureShadowRange");
                return true;
            }
            range.rows = range.sliceBytes / range.rowBytes;
            auto end = range.offset + range.sliceBytes;
            range.touched = requestSize == WholeSize;
            if (!range.touched && requestOffset < memory.allocateInfo.allocationSize)
            {
                auto requestEnd = requestOffset + std::min(requestSize,
                    memory.allocateInfo.allocationSize - requestOffset);
                range.touched = requestOffset < end && requestEnd > range.offset;
            }
            return true;
        }

        // v12: the shadow holds Vulkan-visible BC blocks; the native texture is
        // RGBA8. Decode the whole logical subresource and write it natively.
        inline bool uploadDecoded(ObjectMemory const &memory, DeviceContextLock const &context,
            uint8_t const *data, Range const &range)
        {
            auto const mip = range.nativeSubresource % range.desc.MipLevels;
            auto const width = std::max(1u, range.desc.Width >> mip);
            auto const height = std::max(1u, range.desc.Height >> mip);
            auto const blocksWide = (width + 3u) / 4u;
            auto const blockRows = (height + 3u) / 4u;
            if (range.rowBytes < blocksWide * bc::blockBytes(memory.decodedFrom) || range.rows < blockRows)
            {
                checkError(memory.device, E_INVALIDARG, "TextureShadowDecodedLayout");
                return true;
            }
            auto const pitch = blocksWide * 16u;
            std::vector<uint8_t> decoded(size_t(pitch) * blockRows * 4u);
            if (!bcdecode::rows(memory.decodedFrom, data + range.offset, range.rowBytes, blocksWide, blockRows,
                decoded.data(), pitch))
            {
                checkError(memory.device, E_INVALIDARG, "TextureShadowDecode");
                return true;
            }
            if (range.desc.Usage == D3D11_USAGE_DEFAULT)
            {
                D3D11_BOX box{ 0u, 0u, 0u, width, height, 1u };
                (*context)->UpdateSubresource(range.texture, range.nativeSubresource, &box,
                    decoded.data(), pitch, pitch * blockRows * 4u);
                return true;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            auto access = range.desc.Usage == D3D11_USAGE_DYNAMIC
                ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_READ_WRITE;
            auto hr = (*context)->Map(range.texture, range.nativeSubresource, access, 0u, &mapped);
            if (FAILED(hr) || !mapped.pData || mapped.RowPitch < width * 4u)
            {
                checkError(memory.device, FAILED(hr) ? hr : E_INVALIDARG, "MapTextureShadowDecodedUpload");
                if (SUCCEEDED(hr) && mapped.pData) (*context)->Unmap(range.texture, range.nativeSubresource);
                return true;
            }
            for (UINT row = 0; row < height; ++row)
            {
                std::memcpy(static_cast<uint8_t *>(mapped.pData) + VkDeviceSize(row) * mapped.RowPitch,
                    decoded.data() + VkDeviceSize(row) * pitch, size_t(width) * 4u);
            }
            (*context)->Unmap(range.texture, range.nativeSubresource);
            return true;
        }

        inline bool upload(ObjectMemory const &memory, DeviceContextLock const &context,
            uint8_t const *data, UINT subresource, VkDeviceSize requestOffset, VkDeviceSize requestSize)
        {
            Range range{};
            if (!prepare(memory, subresource, requestOffset, requestSize, range)) return false;
            if (!range.touched || !range.rows) return true;
            if (memory.decodedFrom != VK_FORMAT_UNDEFINED)
            {
                return uploadDecoded(memory, context, data, range);
            }
            if (range.desc.Usage == D3D11_USAGE_DEFAULT)
            {
                // Pipeline-bound textures are not CPU-mappable. Upload the current
                // complete subresource shadow, retaining unchanged bytes in that mip.
                (*context)->UpdateSubresource(range.texture, range.nativeSubresource, nullptr,
                    data + range.offset, range.rowBytes, range.sliceBytes);
                return true;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            auto access = range.desc.Usage == D3D11_USAGE_DYNAMIC
                ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_READ_WRITE;
            auto hr = (*context)->Map(range.texture, range.nativeSubresource, access, 0u, &mapped);
            if (FAILED(hr) || !mapped.pData || mapped.RowPitch < range.rowBytes)
            {
                checkError(memory.device, FAILED(hr) ? hr : E_INVALIDARG, "MapTextureShadowUpload");
                if (SUCCEEDED(hr) && mapped.pData) (*context)->Unmap(range.texture, range.nativeSubresource);
                return true;
            }
            for (UINT row = 0; row < range.rows; ++row)
            {
                std::memcpy(static_cast<uint8_t *>(mapped.pData) + VkDeviceSize(row) * mapped.RowPitch,
                    data + range.offset + VkDeviceSize(row) * range.rowBytes, range.rowBytes);
            }
            (*context)->Unmap(range.texture, range.nativeSubresource);
            return true;
        }

        inline bool download(ObjectMemory const &memory, DeviceContextLock const &context,
            uint8_t *data, UINT subresource, VkDeviceSize requestOffset, VkDeviceSize requestSize)
        {
            Range range{};
            if (!prepare(memory, subresource, requestOffset, requestSize, range)) return false;
            if (!range.touched || !range.rows) return true;
            if (memory.decodedFrom != VK_FORMAT_UNDEFINED)
            {
                // RGBA8 cannot be re-encoded into the original BC blocks. The
                // shadow keeps the last host-written BC bytes, which remain
                // exact unless the GPU wrote the image (only transfers can).
                uint32_t const key[] = { uint32_t(memory.decodedFrom), subresource };
                rdiag::report("bc-decoded-host-readback", rdiag::hashBytes(0x5244u, key, sizeof(key)),
                    "vkFormat=%u subresource=%u action=kept-host-shadow reason=decoded-rgba8-backing-cannot-be-re-encoded",
                    unsigned(memory.decodedFrom), subresource);
                return true;
            }
            ID3D11Texture2D *staging{};
            ID3D11Texture2D *readable = range.texture;
            if (range.desc.Usage != D3D11_USAGE_STAGING)
            {
                auto desc = range.desc;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0u;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.MiscFlags = 0u;
                auto hr = get(memory.device)->getDevice()->CreateTexture2D(&desc, nullptr, &staging);
                if (FAILED(hr) || !staging)
                {
                    checkError(memory.device, FAILED(hr) ? hr : E_INVALIDARG, "CreateTextureShadowReadback");
                    safeRelease(staging);
                    return true;
                }
                (*context)->CopyResource(staging, range.texture);
                readable = staging;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            auto hr = (*context)->Map(readable, range.nativeSubresource, D3D11_MAP_READ, 0u, &mapped);
            if (SUCCEEDED(hr) && mapped.pData && mapped.RowPitch >= range.rowBytes)
            {
                for (UINT row = 0; row < range.rows; ++row)
                {
                    std::memcpy(data + range.offset + VkDeviceSize(row) * range.rowBytes,
                        static_cast<uint8_t const *>(mapped.pData) + VkDeviceSize(row) * mapped.RowPitch,
                        range.rowBytes);
                }
                (*context)->Unmap(readable, range.nativeSubresource);
            }
            else
            {
                checkError(memory.device, FAILED(hr) ? hr : E_INVALIDARG, "MapTextureShadowReadback");
                if (SUCCEEDED(hr) && mapped.pData) (*context)->Unmap(readable, range.nativeSubresource);
            }
            safeRelease(staging);
            return true;
        }
    }
}
