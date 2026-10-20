#define D3D11_NAMESPACE d3d11
#include "D3D11BcCopy.hpp"
#include <cstdio>

int main()
{
    unsigned failures{};
    auto test = [&](char const *name, VkFormat format, VkImageCopy const &copy,
        VkExtent3D src, VkExtent3D dst, bool expected)
    {
        auto actual = ashes::d3d11::bccopy::wholeSubresource(format, copy, src, dst);
        std::printf("%s: %s\n", actual == expected ? "PASS" : "FAIL", name);
        if (actual != expected) ++failures;
    };
    VkImageCopy c{}; c.extent = {6u, 6u, 1u};
    test("complete 6x6 BC7", VK_FORMAT_BC7_UNORM_BLOCK, c, c.extent, c.extent, true);
    c.extent = {3u, 3u, 1u};
    test("complete 3x3 BC7 sRGB", VK_FORMAT_BC7_SRGB_BLOCK, c, c.extent, c.extent, true);
    c.extent = {1u, 1u, 1u};
    test("complete 1x1 BC3", VK_FORMAT_BC3_UNORM_BLOCK, c, c.extent, c.extent, true);
    c.extent = {4u, 4u, 1u};
    test("partial 4x4 region in 8x8", VK_FORMAT_BC7_UNORM_BLOCK, c, {8u,8u,1u}, {8u,8u,1u}, false);
    test("different destination native mip", VK_FORMAT_BC7_UNORM_BLOCK, c, c.extent, {8u,8u,1u}, false);
    test("different source native mip", VK_FORMAT_BC7_UNORM_BLOCK, c, {8u,8u,1u}, c.extent, false);
    test("non-BC unchanged", VK_FORMAT_R8G8B8A8_UNORM, c, c.extent, c.extent, false);
    test("unknown native dimensions", VK_FORMAT_BC7_UNORM_BLOCK, c, {}, {}, false);
    c.srcOffset.x = 4;
    test("nonzero source origin", VK_FORMAT_BC7_UNORM_BLOCK, c, c.extent, c.extent, false);
    c.srcOffset = {}; c.dstOffset.y = 4;
    test("nonzero destination origin", VK_FORMAT_BC7_UNORM_BLOCK, c, c.extent, c.extent, false);
    c.dstOffset = {}; c.extent.depth = 2u;
    test("copy depth mismatch", VK_FORMAT_BC7_UNORM_BLOCK, c, {4u,4u,1u}, {4u,4u,1u}, false);
    std::printf("RESULT: %u selector failures; no GPU/game claims.\n", failures);
    return failures ? 1 : 0;
}
