/*
 * LargeBC7MenuBackgroundProbe - v12 regression for the confirmed v11 black screen.
 *
 * RimWorld 1.7 (Waylith) uploads Resources/Textures/UI/HeroArt/MenuBG_Ideology.jpg
 * as a logical 5333x3000 BC7 image with 13 mips. v11 expanded odd BC images by two
 * hidden mips (21332x12000 native), which exceeds the D3D11 16384 axis limit and
 * returned VK_ERROR_FORMAT_NOT_SUPPORTED at bind time.  v12 stores such images as
 * decoded RGBA8 (logical extent preserved).  This probe checks, through the real
 * Windows loader + ICD:
 *   - image-format query limits (16384 with fallback, 4096 without, storage refused)
 *   - exact 5333x3000 BC7_UNORM 13-mip create/bind/upload
 *   - sampled pixels of mips 0,1,9,11,12 (blit 1:1 NEAREST to RGBA8 + readback)
 *     against an independent CPU decode (bcdec) of the same blocks
 *   - partial (quadrant) uploads with a full-mip bufferRowLength
 *   - decoded->decoded vkCmdCopyImage of a tail mip
 *   - deterministic rejection of vkCmdCopyImageToBuffer from a decoded image
 *   - representation boundaries 4095/4097, 8190/8194, 16383/16384 (x4)
 *   - BC7_SRGB decoded sampling (sRGB->linear, tolerance 1)
 *   - --expect-no-fallback: RW_HD5500_BC_DECODE_FALLBACK=0 behaviour (query limit
 *     4096 and the game-equivalent create/bind -> VK_ERROR_FORMAT_NOT_SUPPORTED)
 * Sandbox runs (Wine) are integration evidence only, not Intel HD 5500 evidence.
 */
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#pragma GCC diagnostic pop

#define INTEL_VENDOR_ID 0x8086u
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define ERR(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

static PFN_vkGetInstanceProcAddr gipa;
#define FN(n) static PFN_##n p_##n;
FN(vkCreateInstance) FN(vkDestroyInstance) FN(vkEnumeratePhysicalDevices) FN(vkGetPhysicalDeviceProperties)
FN(vkGetPhysicalDeviceMemoryProperties) FN(vkGetPhysicalDeviceQueueFamilyProperties) FN(vkGetPhysicalDeviceFeatures)
FN(vkGetPhysicalDeviceFormatProperties) FN(vkGetPhysicalDeviceImageFormatProperties)
FN(vkEnumerateDeviceExtensionProperties) FN(vkCreateDevice) FN(vkGetDeviceProcAddr)
FN(vkDestroyDevice) FN(vkGetDeviceQueue) FN(vkCreateImage) FN(vkDestroyImage) FN(vkGetImageMemoryRequirements)
FN(vkAllocateMemory) FN(vkFreeMemory) FN(vkBindImageMemory) FN(vkCreateBuffer) FN(vkDestroyBuffer)
FN(vkGetBufferMemoryRequirements) FN(vkBindBufferMemory) FN(vkMapMemory) FN(vkUnmapMemory) FN(vkInvalidateMappedMemoryRanges) FN(vkFlushMappedMemoryRanges)
FN(vkCreateCommandPool) FN(vkDestroyCommandPool) FN(vkAllocateCommandBuffers) FN(vkFreeCommandBuffers)
FN(vkBeginCommandBuffer) FN(vkEndCommandBuffer) FN(vkCmdPipelineBarrier) FN(vkCmdCopyBufferToImage)
FN(vkCmdCopyImageToBuffer) FN(vkCmdCopyImage) FN(vkCmdBlitImage) FN(vkCreateFence) FN(vkDestroyFence)
FN(vkQueueSubmit) FN(vkWaitForFences) FN(vkDeviceWaitIdle)
#undef FN

static VkPhysicalDevice g_physical;
static VkDevice g_device;
static VkQueue g_queue;
static uint32_t g_family = UINT32_MAX;
static VkPhysicalDeviceMemoryProperties g_mp;
static VkCommandPool g_pool;
static int g_failures;
static int g_cases;
/* Default mirrors the passing retained atlas probe; --direct-destination adds STORAGE (direct UAV blit path). */
static VkImageUsageFlags g_verify_dst_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

static const char *rn(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default: return "unrecognized VkResult";
    }
}

static void case_result(int ok, const char *name, const char *detail) {
    ++g_cases;
    if (ok) LOG("CASE PASS: %s %s\n", name, detail ? detail : "");
    else { ++g_failures; LOG("CASE FAIL: %s %s\n", name, detail ? detail : ""); }
}

static int is_abs(const wchar_t *p) {
    return p && ((p[0] == L'\\' && p[1] == L'\\') ||
        (iswalpha((wint_t)p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')));
}
static int ci_contains(const char *text, const char *part) {
    size_t n = strlen(part);
    for (; *text; ++text) {
        size_t i;
        for (i = 0; i < n && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)part[i]); ++i) {}
        if (i == n) return 1;
    }
    return 0;
}
static int looks_software(const VkPhysicalDeviceProperties *p) {
    return p->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU || ci_contains(p->deviceName, "warp") ||
        ci_contains(p->deviceName, "swiftshader") || ci_contains(p->deviceName, "lavapipe") ||
        ci_contains(p->deviceName, "llvmpipe") || ci_contains(p->deviceName, "software");
}

/* ---------------------------------------------------------------- BC7 data */
static uint32_t mip_dim(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1u; }
static uint32_t blocks(uint32_t v) { return (v + 3u) / 4u; }
static uint32_t full_chain(uint32_t w, uint32_t h) {
    uint32_t m = w > h ? w : h, n = 1;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

typedef struct Payload {
    uint32_t w, h, mips;
    VkDeviceSize offsets[16];
    VkDeviceSize sizes[16];
    VkDeviceSize total;
    uint8_t *data;
} Payload;

static uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x ? x : 0x9E3779B9u; }

/* Valid BC7 blocks cycling all eight modes; every non-reserved bit pattern is legal. */
static int make_payload(Payload *p, uint32_t w, uint32_t h, uint32_t mips, uint32_t seed) {
    uint32_t l; VkDeviceSize off = 0;
    memset(p, 0, sizeof(*p));
    p->w = w; p->h = h; p->mips = mips;
    for (l = 0; l < mips; ++l) {
        p->offsets[l] = off;
        p->sizes[l] = (VkDeviceSize)blocks(mip_dim(w, l)) * blocks(mip_dim(h, l)) * 16u;
        off += p->sizes[l];
    }
    p->total = off;
    p->data = (uint8_t *)malloc((size_t)off);
    if (!p->data) return 0;
    for (l = 0; l < mips; ++l) {
        uint8_t *b = p->data + p->offsets[l];
        VkDeviceSize n = p->sizes[l] / 16u, i;
        uint32_t s = seed ^ (l * 0x85EBCA6Bu) ^ 0xC2B2AE35u;
        for (i = 0; i < n; ++i) {
            uint32_t mode = (uint32_t)((i + l) % 8u), j;
            uint8_t *blk = b + i * 16u;
            for (j = 0; j < 16u; ++j) blk[j] = (uint8_t)(xs32(&s) >> 11);
            if (mode == 7u) blk[0] = 0x80u;
            else blk[0] = (uint8_t)((1u << mode) | (blk[0] & (uint8_t)~((2u << mode) - 1u)));
        }
    }
    return 1;
}

/* Independent CPU decode of one logical mip into tight RGBA8. */
static uint8_t *decode_mip(const Payload *p, uint32_t l) {
    uint32_t w = mip_dim(p->w, l), h = mip_dim(p->h, l), bw = blocks(w), bh = blocks(h), bx, by, y;
    uint8_t *out = (uint8_t *)malloc((size_t)w * h * 4u), tmp[64];
    if (!out) return NULL;
    for (by = 0; by < bh; ++by)
        for (bx = 0; bx < bw; ++bx) {
            bcdec_bc7(p->data + p->offsets[l] + ((VkDeviceSize)by * bw + bx) * 16u, tmp, 16);
            for (y = 0; y < 4u && by * 4u + y < h; ++y) {
                uint32_t cols = (bx * 4u + 4u <= w) ? 4u : (w - bx * 4u);
                memcpy(out + (((size_t)(by * 4u + y) * w) + bx * 4u) * 4u, tmp + y * 16u, cols * 4u);
            }
        }
    return out;
}

static uint8_t srgb_to_linear8(uint8_t c) {
    double v = c / 255.0, lin = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    return (uint8_t)floor(lin * 255.0 + 0.5);
}

/* -------------------------------------------------------------- resources */
static int find_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    uint32_t i;
    for (i = 0; i < g_mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_mp.memoryTypes[i].propertyFlags & want) == want &&
            !(g_mp.memoryTypes[i].propertyFlags & avoid)) return (int)i;
    for (i = 0; i < g_mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

typedef struct Img { VkImage image; VkDeviceMemory memory; VkResult createResult, bindResult; } Img;
typedef struct Buf { VkBuffer buffer; VkDeviceMemory memory; uint8_t *mapped; VkDeviceSize size; } Buf;

static VkResult create_image(Img *img, VkFormat fmt, uint32_t w, uint32_t h, uint32_t mips, VkImageUsageFlags usage, const char *name) {
    VkImageCreateInfo ii; VkMemoryRequirements req; VkMemoryAllocateInfo ai; int type; VkResult r;
    memset(img, 0, sizeof(*img)); memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1; ii.mipLevels = mips; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = img->createResult = p_vkCreateImage(g_device, &ii, NULL, &img->image);
    LOG("IMAGE_CREATE: %s format=%d %ux%u mips=%u usage=0x%X => %s (%d)\n", name, (int)fmt, w, h, mips, usage, rn(r), (int)r);
    if (r != VK_SUCCESS) { img->image = VK_NULL_HANDLE; return r; }
    p_vkGetImageMemoryRequirements(g_device, img->image, &req);
    type = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    LOG("IMAGE_REQUIREMENTS: %s bytes=%llu align=%llu bits=0x%X type=%d\n", name, (unsigned long long)req.size,
        (unsigned long long)req.alignment, req.memoryTypeBits, type);
    if (type < 0) return img->bindResult = VK_ERROR_FEATURE_NOT_PRESENT;
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)type;
    r = p_vkAllocateMemory(g_device, &ai, NULL, &img->memory);
    if (r != VK_SUCCESS) { img->memory = VK_NULL_HANDLE; LOG("IMAGE_ALLOC: %s => %s\n", name, rn(r)); return img->bindResult = r; }
    r = img->bindResult = p_vkBindImageMemory(g_device, img->image, img->memory, 0);
    LOG("IMAGE_BIND: %s => %s (%d)\n", name, rn(r), (int)r);
    return r;
}
static void destroy_image(Img *img) {
    if (img->image) p_vkDestroyImage(g_device, img->image, NULL);
    if (img->memory) p_vkFreeMemory(g_device, img->memory, NULL);
    memset(img, 0, sizeof(*img));
}
static VkResult create_buffer_ex(Buf *b, VkDeviceSize size, VkBufferUsageFlags usage, int map_now) {
    VkBufferCreateInfo bi; VkMemoryRequirements req; VkMemoryAllocateInfo ai; int type; VkResult r;
    memset(b, 0, sizeof(*b)); memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = p_vkCreateBuffer(g_device, &bi, NULL, &b->buffer);
    if (r != VK_SUCCESS) { b->buffer = VK_NULL_HANDLE; return r; }
    p_vkGetBufferMemoryRequirements(g_device, b->buffer, &req);
    type = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (type < 0) return VK_ERROR_FEATURE_NOT_PRESENT;
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)type;
    r = p_vkAllocateMemory(g_device, &ai, NULL, &b->memory);
    if (r != VK_SUCCESS) { b->memory = VK_NULL_HANDLE; return r; }
    r = p_vkBindBufferMemory(g_device, b->buffer, b->memory, 0);
    if (r != VK_SUCCESS) return r;
    b->size = size;
    if (!map_now) return VK_SUCCESS;
    return p_vkMapMemory(g_device, b->memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->mapped);
}
static VkResult create_buffer(Buf *b, VkDeviceSize size, VkBufferUsageFlags usage) { return create_buffer_ex(b, size, usage, 1); }
/* Upload pattern used by the retained v11 probes: map, write, flush, unmap before submission.
   g_persistent_upload=1 instead keeps the HOST_COHERENT mapping open across the submission. */
static int g_persistent_upload;
static VkResult create_upload_buffer(Buf *b, const void *data, VkDeviceSize size) {
    VkResult r = create_buffer_ex(b, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
    VkMappedMemoryRange range;
    if (r != VK_SUCCESS || !b->mapped) return r == VK_SUCCESS ? VK_ERROR_MEMORY_MAP_FAILED : r;
    memcpy(b->mapped, data, (size_t)size);
    if (g_persistent_upload) return VK_SUCCESS;
    memset(&range, 0, sizeof(range)); range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = b->memory; range.size = VK_WHOLE_SIZE;
    r = p_vkFlushMappedMemoryRanges(g_device, 1u, &range);
    p_vkUnmapMemory(g_device, b->memory); b->mapped = NULL;
    return r;
}
/* Readback pattern used by the retained v11 probes: map after the fence, then invalidate. */
static VkResult map_for_read(Buf *b) {
    VkMappedMemoryRange range; VkResult r = p_vkMapMemory(g_device, b->memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->mapped);
    if (r != VK_SUCCESS) { b->mapped = NULL; return r; }
    memset(&range, 0, sizeof(range)); range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = b->memory; range.size = VK_WHOLE_SIZE;
    return p_vkInvalidateMappedMemoryRanges(g_device, 1u, &range);
}
static void destroy_buffer(Buf *b) {
    if (b->mapped) p_vkUnmapMemory(g_device, b->memory);
    if (b->buffer) p_vkDestroyBuffer(g_device, b->buffer, NULL);
    if (b->memory) p_vkFreeMemory(g_device, b->memory, NULL);
    memset(b, 0, sizeof(*b));
}

static VkCommandBuffer begin_cmd(void) {
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1u };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (p_vkAllocateCommandBuffers(g_device, &cai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (p_vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { p_vkFreeCommandBuffers(g_device, g_pool, 1u, &cmd); return VK_NULL_HANDLE; }
    return cmd;
}
/* Ends, submits with a fence and waits.  Returns the first non-success result. */
static VkResult end_submit_wait(VkCommandBuffer cmd, const char *what) {
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0u };
    VkFence fence = VK_NULL_HANDLE; VkSubmitInfo si; VkResult r = p_vkEndCommandBuffer(cmd);
    LOG("END: %s => %s (%d)\n", what, rn(r), (int)r);
    if (r == VK_SUCCESS) r = p_vkCreateFence(g_device, &fi, NULL, &fence);
    if (r == VK_SUCCESS) {
        memset(&si, 0, sizeof(si)); si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1u; si.pCommandBuffers = &cmd;
        r = p_vkQueueSubmit(g_queue, 1u, &si, fence);
        LOG("SUBMIT: %s => %s (%d)\n", what, rn(r), (int)r);
    }
    if (r == VK_SUCCESS) {
        r = p_vkWaitForFences(g_device, 1u, &fence, VK_TRUE, UINT64_C(60000000000));
        LOG("WAIT: %s => %s (%d)\n", what, rn(r), (int)r);
    }
    if (fence) p_vkDestroyFence(g_device, fence, NULL);
    p_vkFreeCommandBuffers(g_device, g_pool, 1u, &cmd);
    return r;
}
static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
    VkAccessFlags src, VkAccessFlags dst, uint32_t baseMip, uint32_t levels) {
    VkImageMemoryBarrier b; memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.oldLayout = from; b.newLayout = to;
    b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = baseMip; b.subresourceRange.levelCount = levels; b.subresourceRange.layerCount = 1u;
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static VkBufferImageCopy region(VkDeviceSize off, uint32_t mip, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t rowLen, uint32_t imgH) {
    VkBufferImageCopy r; memset(&r, 0, sizeof(r));
    r.bufferOffset = off; r.bufferRowLength = rowLen; r.bufferImageHeight = imgH;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.imageSubresource.mipLevel = mip; r.imageSubresource.layerCount = 1u;
    r.imageOffset.x = x; r.imageOffset.y = y; r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = 1u;
    return r;
}

/* Uploads every mip of payload p (tight) into img; leaves it in TRANSFER_SRC_OPTIMAL. */
static VkResult upload_all(Img *img, const Payload *p, const char *name) {
    Buf up; VkCommandBuffer cmd; VkBufferImageCopy regs[16]; uint32_t l; VkResult r;
    r = create_upload_buffer(&up, p->data, p->total);
    if (r != VK_SUCCESS) { destroy_buffer(&up); return r; }
    for (l = 0; l < p->mips; ++l) regs[l] = region(p->offsets[l], l, 0, 0, mip_dim(p->w, l), mip_dim(p->h, l), 0, 0);
    cmd = begin_cmd();
    if (!cmd) { destroy_buffer(&up); return VK_ERROR_INITIALIZATION_FAILED; }
    barrier(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0, p->mips);
    p_vkCmdCopyBufferToImage(cmd, up.buffer, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, p->mips, regs);
    barrier(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0, p->mips);
    r = end_submit_wait(cmd, name);
    destroy_buffer(&up);
    return r;
}

/* Samples mip `mip` of src (TRANSFER_SRC layout) with a 1:1 NEAREST blit into an RGBA8
   image, reads it back and compares with the CPU decode.  srgb => linearised compare. */
static int verify_mip(VkImage src, const Payload *p, uint32_t mip, int srgb, const char *name) {
    uint32_t w = mip_dim(p->w, mip), h = mip_dim(p->h, mip), x, y, bad = 0, maxdiff = 0;
    Img dst; Buf rb; VkCommandBuffer cmd; VkImageBlit blit; VkBufferImageCopy rr; VkResult r;
    uint8_t *expected = decode_mip(p, mip); char detail[256]; int ok = 0;
    memset(&dst, 0, sizeof(dst)); memset(&rb, 0, sizeof(rb));
    if (!expected) { case_result(0, name, "host allocation failed"); return 0; }
    r = create_image(&dst, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1u,
        g_verify_dst_usage, "verify-destination");
    if (r == VK_SUCCESS) r = create_buffer_ex(&rb, (VkDeviceSize)w * h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    if (r != VK_SUCCESS) { snprintf(detail, sizeof(detail), "setup %s", rn(r)); goto done; }
    cmd = begin_cmd();
    if (!cmd) { snprintf(detail, sizeof(detail), "command buffer"); goto done; }
    barrier(cmd, dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1);
    memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; blit.srcSubresource.mipLevel = mip; blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)w; blit.srcOffsets[1].y = (int32_t)h; blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource; blit.dstSubresource.mipLevel = 0;
    blit.dstOffsets[1] = blit.srcOffsets[1];
    p_vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0, 1);
    rr = region(0, 0, 0, 0, w, h, 0, 0);
    p_vkCmdCopyImageToBuffer(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &rr);
    {   /* Required by Vulkan before host reads of device writes (TRANSFER_WRITE -> HOST_READ). */
        VkBufferMemoryBarrier hb; memset(&hb, 0, sizeof(hb));
        hb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; hb.buffer = rb.buffer; hb.size = VK_WHOLE_SIZE;
        p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
    }
    r = end_submit_wait(cmd, name);
    if (r != VK_SUCCESS) { snprintf(detail, sizeof(detail), "submission %s", rn(r)); goto done; }
    r = map_for_read(&rb);
    if (r != VK_SUCCESS || !rb.mapped) { snprintf(detail, sizeof(detail), "map/invalidate after fence %s", rn(r)); goto done; }
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            const uint8_t *g = rb.mapped + ((size_t)y * w + x) * 4u, *e = expected + ((size_t)y * w + x) * 4u;
            uint32_t c;
            for (c = 0; c < 4u; ++c) {
                int want = (srgb && c < 3u) ? srgb_to_linear8(e[c]) : e[c];
                uint32_t d = (uint32_t)abs((int)g[c] - want);
                if (d > maxdiff) maxdiff = d;
                if (d > (srgb ? 1u : 0u)) {
                    if (bad < 6u) LOG("MISMATCH: %s mip=%u x=%u y=%u c=%u got=%u want=%d\n", name, mip, x, y, c, g[c], want);
                    ++bad; break;
                }
            }
        }
    ok = bad == 0;
    snprintf(detail, sizeof(detail), "mip=%u extent=%ux%u texels=%u badTexels=%u maxChannelDiff=%u compare=%s", mip, w, h, w * h, bad, maxdiff,
        srgb ? "srgb-linearised(tol=1)" : "exact");
done:
    case_result(ok, name, detail);
    destroy_buffer(&rb); destroy_image(&dst); free(expected);
    return ok;
}

/* ------------------------------------------------------------------ cases */
static VkImageUsageFlags kSampledTransfer = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static void query_cases(int expect_fallback) {
    VkImageFormatProperties ip; VkResult r; char d[256]; VkFormatProperties fp;
    memset(&ip, 0, sizeof(ip));
    r = p_vkGetPhysicalDeviceImageFormatProperties(g_physical, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, kSampledTransfer, 0, &ip);
    snprintf(d, sizeof(d), "BC7_UNORM sampled|transfer => %s maxExtent=%ux%u maxMips=%u samples=0x%X", rn(r), ip.maxExtent.width, ip.maxExtent.height, ip.maxMipLevels, ip.sampleCounts);
    LOG("QUERY: %s\n", d);
    if (expect_fallback) case_result(r == VK_SUCCESS && ip.maxExtent.width == 16384u && ip.maxExtent.height == 16384u && ip.maxMipLevels >= 13u && ip.sampleCounts == VK_SAMPLE_COUNT_1_BIT, "query-bc7-limit-16384", d);
    else case_result(r == VK_SUCCESS && ip.maxExtent.width == 4096u && ip.maxExtent.height == 4096u, "query-bc7-limit-4096-no-fallback", d);
    memset(&ip, 0, sizeof(ip));
    r = p_vkGetPhysicalDeviceImageFormatProperties(g_physical, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, kSampledTransfer | VK_IMAGE_USAGE_STORAGE_BIT, 0, &ip);
    snprintf(d, sizeof(d), "BC7_UNORM +storage => %s", rn(r));
    case_result(r == VK_ERROR_FORMAT_NOT_SUPPORTED, "query-bc7-storage-refused", d);
    memset(&ip, 0, sizeof(ip));
    r = p_vkGetPhysicalDeviceImageFormatProperties(g_physical, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, kSampledTransfer, 0, &ip);
    snprintf(d, sizeof(d), "BC6H_UFLOAT (not decodable) => %s maxExtent=%ux%u", rn(r), ip.maxExtent.width, ip.maxExtent.height);
    case_result(r != VK_SUCCESS || (ip.maxExtent.width <= 4096u && ip.maxExtent.height <= 4096u), "query-bc6h-not-extended", d);
    memset(&fp, 0, sizeof(fp));
    p_vkGetPhysicalDeviceFormatProperties(g_physical, VK_FORMAT_BC7_UNORM_BLOCK, &fp);
    snprintf(d, sizeof(d), "BC7_UNORM optimalFeatures=0x%X bufferFeatures=0x%X", fp.optimalTilingFeatures, fp.bufferFeatures);
    case_result((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
        !(fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) &&
        fp.bufferFeatures == 0u, "features-bc7-conservative", d);
}

static void menu_background_cases(void) {
    Payload p; Img a, b; VkResult r; char d[256];
    static const uint32_t verify_mips[] = { 0u, 1u, 9u, 11u, 12u };
    uint32_t i;
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    if (!make_payload(&p, 5333u, 3000u, 13u, 0x5333u)) { case_result(0, "menu-bg-payload", "host allocation"); return; }
    LOG("MENU_BG: logical 5333x3000 BC7_UNORM mips=13 payloadBytes=%llu mip0Bytes=%llu (v11 native would be 21332x12000)\n",
        (unsigned long long)p.total, (unsigned long long)p.sizes[0]);
    r = create_image(&a, VK_FORMAT_BC7_UNORM_BLOCK, 5333u, 3000u, 13u, kSampledTransfer, "menu-bg-A");
    snprintf(d, sizeof(d), "create=%s bind=%s", rn(a.createResult), rn(a.bindResult));
    case_result(r == VK_SUCCESS, "menu-bg-create-bind-5333x3000-13mips", d);
    if (r != VK_SUCCESS) goto done;
    r = upload_all(&a, &p, "menu-bg-upload-all-13-mips");
    snprintf(d, sizeof(d), "=> %s", rn(r));
    case_result(r == VK_SUCCESS, "menu-bg-upload-13-mips", d);
    if (r != VK_SUCCESS) goto done;
    for (i = 0; i < sizeof(verify_mips) / sizeof(verify_mips[0]); ++i) {
        char name[64]; snprintf(name, sizeof(name), "menu-bg-sampled-mip%u", verify_mips[i]);
        verify_mip(a.image, &p, verify_mips[i], 0, name);
    }

    /* Image B: quadrant uploads of mip 0 using the full-mip row pitch, mip 1 tight, mip 9 via copy from A. */
    r = create_image(&b, VK_FORMAT_BC7_UNORM_BLOCK, 5333u, 3000u, 13u, kSampledTransfer, "menu-bg-B");
    case_result(r == VK_SUCCESS, "menu-bg-second-image-create-bind", rn(r));
    if (r == VK_SUCCESS) {
        Buf up; VkCommandBuffer cmd; VkBufferImageCopy regs[5]; VkImageCopy ic;
        const uint32_t bw = blocks(5333u), qx = 2664u, qy = 1496u;
        memset(&up, 0, sizeof(up));
        r = create_upload_buffer(&up, p.data, p.offsets[2]);
        if (r == VK_SUCCESS) {
            regs[0] = region(0, 0, 0, 0, qx, qy, bw * 4u, 3000u);
            regs[1] = region((VkDeviceSize)(qx / 4u) * 16u, 0, (int32_t)qx, 0, 5333u - qx, qy, bw * 4u, 3000u);
            regs[2] = region((VkDeviceSize)(qy / 4u) * bw * 16u, 0, 0, (int32_t)qy, qx, 3000u - qy, bw * 4u, 3000u);
            regs[3] = region(((VkDeviceSize)(qy / 4u) * bw + qx / 4u) * 16u, 0, (int32_t)qx, (int32_t)qy, 5333u - qx, 3000u - qy, bw * 4u, 3000u);
            regs[4] = region(p.offsets[1], 1, 0, 0, mip_dim(5333u, 1), mip_dim(3000u, 1), 0, 0);
            cmd = begin_cmd();
            barrier(cmd, b.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 13u);
            p_vkCmdCopyBufferToImage(cmd, up.buffer, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 5u, regs);
            memset(&ic, 0, sizeof(ic));
            ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.mipLevel = 9u; ic.srcSubresource.layerCount = 1u;
            ic.dstSubresource = ic.srcSubresource;
            ic.extent.width = mip_dim(5333u, 9u); ic.extent.height = mip_dim(3000u, 9u); ic.extent.depth = 1u;
            p_vkCmdCopyImage(cmd, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &ic);
            barrier(cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0, 13u);
            r = end_submit_wait(cmd, "menu-bg-B-quadrants-and-copy");
        }
        destroy_buffer(&up);
        snprintf(d, sizeof(d), "4 quadrant regions (rowLength=%u) + mip1 + vkCmdCopyImage mip9 decoded->decoded => %s", bw * 4u, rn(r));
        case_result(r == VK_SUCCESS, "menu-bg-partial-upload-and-copy-submit", d);
        if (r == VK_SUCCESS) {
            verify_mip(b.image, &p, 0u, 0, "menu-bg-quadrant-upload-mip0");
            verify_mip(b.image, &p, 1u, 0, "menu-bg-tight-upload-mip1");
            verify_mip(b.image, &p, 9u, 0, "menu-bg-decoded-copy-mip9");
        }
    }

    /* Readback of a decoded image must be refused deterministically, not return wrong bytes. */
    {
        Buf rb; VkCommandBuffer cmd; VkBufferImageCopy rr = region(0, 12u, 0, 0, 1u, 1u, 0, 0); VkResult er;
        memset(&rb, 0, sizeof(rb));
        r = create_buffer(&rb, 16u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        cmd = r == VK_SUCCESS ? begin_cmd() : VK_NULL_HANDLE;
        if (cmd) {
            p_vkCmdCopyImageToBuffer(cmd, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1u, &rr);
            er = p_vkEndCommandBuffer(cmd);
            p_vkFreeCommandBuffers(g_device, g_pool, 1u, &cmd);
            snprintf(d, sizeof(d), "vkEndCommandBuffer after vkCmdCopyImageToBuffer(decoded) => %s (%d)", rn(er), (int)er);
            case_result(er == VK_ERROR_FORMAT_NOT_SUPPORTED, "menu-bg-readback-refused", d);
        } else case_result(0, "menu-bg-readback-refused", "setup failed");
        destroy_buffer(&rb);
    }
done:
    destroy_image(&b); destroy_image(&a); free(p.data);
}

static void boundary_cases(void) {
    static const struct { uint32_t w, h; const char *expect; } k[] = {
        { 4095u, 4u, "native-prefix2(16380)" }, { 4097u, 4u, "decoded(16388>16384)" },
        { 8190u, 4u, "native-prefix1(16380)" }, { 8194u, 4u, "decoded(16388>16384)" },
        { 16383u, 4u, "decoded(odd)" },        { 16384u, 4u, "native-prefix0" },
    };
    uint32_t i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); ++i) {
        Payload p; Img img; VkResult r; char name[96], d[160]; uint32_t mips = full_chain(k[i].w, k[i].h);
        memset(&img, 0, sizeof(img));
        if (!make_payload(&p, k[i].w, k[i].h, mips, 0xB0u + i)) { case_result(0, "boundary-payload", "alloc"); continue; }
        snprintf(name, sizeof(name), "boundary-%ux%u-%s", k[i].w, k[i].h, k[i].expect);
        r = create_image(&img, VK_FORMAT_BC7_UNORM_BLOCK, k[i].w, k[i].h, mips, kSampledTransfer, name);
        if (r == VK_SUCCESS) r = upload_all(&img, &p, name);
        snprintf(d, sizeof(d), "mips=%u create/bind/upload => %s", mips, rn(r));
        case_result(r == VK_SUCCESS, name, d);
        if (r == VK_SUCCESS) {
            char v[112];
            snprintf(v, sizeof(v), "%s-mip0", name); verify_mip(img.image, &p, 0u, 0, v);
            snprintf(v, sizeof(v), "%s-mip%u", name, mips - 1u); verify_mip(img.image, &p, mips - 1u, 0, v);
        }
        destroy_image(&img); free(p.data);
    }
}

static void srgb_case(void) {
    Payload p; Img img; VkResult r; uint32_t mips = full_chain(4097u, 8u);
    memset(&img, 0, sizeof(img));
    if (!make_payload(&p, 4097u, 8u, mips, 0x5A5Au)) { case_result(0, "srgb-payload", "alloc"); return; }
    r = create_image(&img, VK_FORMAT_BC7_SRGB_BLOCK, 4097u, 8u, mips, kSampledTransfer, "srgb-4097x8");
    if (r == VK_SUCCESS) r = upload_all(&img, &p, "srgb-4097x8-upload");
    case_result(r == VK_SUCCESS, "srgb-decoded-create-upload", rn(r));
    if (r == VK_SUCCESS) {
        verify_mip(img.image, &p, 0u, 1, "srgb-decoded-sampled-mip0");
        verify_mip(img.image, &p, 3u, 1, "srgb-decoded-sampled-mip3");
    }
    destroy_image(&img); free(p.data);
}

static void no_fallback_cases(void) {
    Img img; VkResult r; char d[160];
    LOG("NO_FALLBACK: game-equivalent create/bind of 5333x3000 BC7 beyond the reported 4096 limit (mirrors RimWorld's call; invalid usage by spec).\n");
    r = create_image(&img, VK_FORMAT_BC7_UNORM_BLOCK, 5333u, 3000u, 13u, kSampledTransfer, "menu-bg-no-fallback");
    snprintf(d, sizeof(d), "create=%s bind=%s", rn(img.createResult), rn(img.bindResult));
    case_result(r == VK_ERROR_FORMAT_NOT_SUPPORTED, "no-fallback-deterministic-format-not-supported", d);
    destroy_image(&img);
}


/* Control: RGBA8 source with known texels through (a) direct readback and (b) 1:1 and scaled-by-1 blit + readback.
   Separates bridge sequencing problems from BC/decoded representation problems. */
static int readback_rgba8(VkImage img, uint32_t w, uint32_t h, int blit_first, int scale2, uint8_t *out, const char *name) {
    Img dst; Buf rb; VkCommandBuffer cmd; VkBufferImageCopy rr = region(0, 0, 0, 0, w, h, 0, 0); VkResult r; VkImage readFrom = img;
    memset(&dst, 0, sizeof(dst)); memset(&rb, 0, sizeof(rb));
    if (blit_first && create_image(&dst, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1u, g_verify_dst_usage, "control-dst") != VK_SUCCESS) return 0;
    if (create_buffer_ex(&rb, (VkDeviceSize)w * h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0) != VK_SUCCESS) { destroy_image(&dst); return 0; }
    cmd = begin_cmd();
    if (blit_first) {
        VkImageBlit b; memset(&b, 0, sizeof(b));
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
        b.srcOffsets[1].x = (int32_t)(scale2 ? w / 2u : w); b.srcOffsets[1].y = (int32_t)(scale2 ? h / 2u : h); b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = (int32_t)w; b.dstOffsets[1].y = (int32_t)h; b.dstOffsets[1].z = 1;
        barrier(cmd, dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1);
        p_vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        barrier(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0, 1);
        readFrom = dst.image;
    }
    p_vkCmdCopyImageToBuffer(cmd, readFrom, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &rr);
    {   VkBufferMemoryBarrier hb; memset(&hb, 0, sizeof(hb));
        hb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; hb.buffer = rb.buffer; hb.size = VK_WHOLE_SIZE;
        p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL); }
    r = end_submit_wait(cmd, name);
    if (r == VK_SUCCESS) r = map_for_read(&rb);
    if (r == VK_SUCCESS && rb.mapped) memcpy(out, rb.mapped, (size_t)w * h * 4u);
    destroy_buffer(&rb); destroy_image(&dst);
    return r == VK_SUCCESS;
}
static void control_cases(void) {
    const uint32_t w = 8u, h = 4u; Img src; Buf up; VkCommandBuffer cmd; VkBufferImageCopy rg = region(0, 0, 0, 0, w, h, 0, 0);
    uint8_t data[8 * 4 * 4], got[8 * 4 * 4]; uint32_t i; char d[200]; VkResult r;
    for (i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 7u + 3u);
    memset(&src, 0, sizeof(src)); memset(&up, 0, sizeof(up));
    if (create_image(&src, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1u, kSampledTransfer, "control-src") != VK_SUCCESS) { case_result(0, "control-setup", "image"); return; }
    r = create_upload_buffer(&up, data, sizeof(data));
    if (r == VK_SUCCESS) {
        cmd = begin_cmd();
        barrier(cmd, src.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1);
        p_vkCmdCopyBufferToImage(cmd, up.buffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        barrier(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0, 1);
        r = end_submit_wait(cmd, "control-upload");
    }
    destroy_buffer(&up);
    memset(got, 0xEE, sizeof(got));
    i = readback_rgba8(src.image, w, h, 0, 0, got, "control-direct-readback");
    snprintf(d, sizeof(d), "upload=%s readback=%u first=%u,%u,%u,%u want=%u,%u,%u,%u", rn(r), i, got[0], got[1], got[2], got[3], data[0], data[1], data[2], data[3]);
    case_result(i && !memcmp(got, data, sizeof(data)), "control-rgba8-direct-readback", d);
    memset(got, 0xEE, sizeof(got));
    i = readback_rgba8(src.image, w, h, 1, 0, got, "control-blit-1to1");
    snprintf(d, sizeof(d), "first=%u,%u,%u,%u want=%u,%u,%u,%u", got[0], got[1], got[2], got[3], data[0], data[1], data[2], data[3]);
    case_result(i && !memcmp(got, data, sizeof(data)), "control-rgba8-blit-1to1", d);
    memset(got, 0xEE, sizeof(got));
    i = readback_rgba8(src.image, w, h, 1, 1, got, "control-blit-2x");
    snprintf(d, sizeof(d), "first=%u,%u,%u,%u want=%u,%u,%u,%u", got[0], got[1], got[2], got[3], data[0], data[1], data[2], data[3]);
    case_result(i && !memcmp(got, data, 4), "control-rgba8-blit-2x-upscale", d);
    destroy_image(&src);
}

static void usage(const wchar_t *exe) {
    LOG("Usage: %ls --loader C:\\abs\\vulkan-1.dll --icd C:\\abs\\ashesD3D11Renderer_icd.json [--allow-non-intel-test] [--expect-no-fallback] [--quick] [--only menu|boundary|srgb] [--direct-destination] [--persistent-upload]\n", exe);
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL; const wchar_t *only = NULL; int allow_non_intel = 0, expect_no_fallback = 0, quick = 0, i, status = 1;
    HMODULE loader = NULL; union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } l;
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice list[16]; uint32_t count = 16; VkPhysicalDeviceProperties props;
    const char *first_failure = NULL;
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--expect-no-fallback")) expect_no_fallback = 1;
        else if (!wcscmp(argv[i], L"--quick")) quick = 1;
        else if (!wcscmp(argv[i], L"--persistent-upload")) g_persistent_upload = 1;
        else if (!wcscmp(argv[i], L"--direct-destination")) g_verify_dst_usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        else if (!wcscmp(argv[i], L"--only") && i + 1 < argc) only = argv[++i];
        else if (!wcscmp(argv[i], L"--help")) { usage(argv[0]); return 0; }
        else { ERR("FAIL: unrecognized argument %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!is_abs(loader_path) || !is_abs(icd_path)) { ERR("FAIL: absolute --loader and --icd are mandatory.\n"); return 2; }
    if (GetModuleHandleW(L"vulkan-1.dll")) { ERR("FAIL: vulkan-1.dll already loaded.\n"); return 3; }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path) || !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) { ERR("FAIL: could not set ICD variables.\n"); return 3; }
    LOG("LargeBC7MenuBackgroundProbe (v12)\nLOADER: %ls\nICD: %ls\nMODE: %s%s\n", loader_path, icd_path,
        expect_no_fallback ? "expect-no-fallback (RW_HD5500_BC_DECODE_FALLBACK=0)" : "default (decode fallback enabled)", quick ? " quick" : "");
    if (allow_non_intel) LOG("WARNING: --allow-non-intel-test relaxes only the Intel vendor gate (sandbox integration). This is not HD 5500 evidence.\n");
    loader = LoadLibraryW(loader_path);
    if (!loader) { status = 4; first_failure = "LoadLibraryW(loader)"; goto cleanup; }
    l.raw = GetProcAddress(loader, "vkGetInstanceProcAddr"); gipa = l.typed;
    if (!gipa) { status = 4; first_failure = "vkGetInstanceProcAddr"; goto cleanup; }
    p_vkCreateInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "LargeBC7MenuBackgroundProbe", 12u, "standalone", 1u, VK_API_VERSION_1_0 };
        VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0u, &app, 0u, NULL, 0u, NULL };
        VkResult r = p_vkCreateInstance ? p_vkCreateInstance(&ci, NULL, &instance) : VK_ERROR_INITIALIZATION_FAILED;
        LOG("RESULT: vkCreateInstance => %s (%d)\n", rn(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateInstance"; goto cleanup; }
    }
#define LI(n) p_##n = (PFN_##n)gipa(instance, #n); if (!p_##n) { status = 20; first_failure = #n; goto cleanup; }
    LI(vkDestroyInstance) LI(vkEnumeratePhysicalDevices) LI(vkGetPhysicalDeviceProperties) LI(vkGetPhysicalDeviceMemoryProperties)
    LI(vkGetPhysicalDeviceQueueFamilyProperties) LI(vkGetPhysicalDeviceFeatures) LI(vkGetPhysicalDeviceFormatProperties)
    LI(vkGetPhysicalDeviceImageFormatProperties) LI(vkEnumerateDeviceExtensionProperties) LI(vkCreateDevice) LI(vkGetDeviceProcAddr)
#undef LI
    if (p_vkEnumeratePhysicalDevices(instance, &count, list) < 0 || !count) { status = 5; first_failure = "vkEnumeratePhysicalDevices"; goto cleanup; }
    for (i = 0; i < (int)count && !g_physical; ++i) {
        VkPhysicalDeviceProperties q; uint32_t n = 0, f; VkQueueFamilyProperties qf[16];
        p_vkGetPhysicalDeviceProperties(list[i], &q);
        LOG("ADAPTER[%d]: name=%s vendor=0x%04X device=0x%04X%s\n", i, q.deviceName, q.vendorID, q.deviceID, looks_software(&q) ? " [software-like]" : "");
        if (looks_software(&q) || (!allow_non_intel && q.vendorID != INTEL_VENDOR_ID)) continue;
        p_vkGetPhysicalDeviceQueueFamilyProperties(list[i], &n, NULL); if (n > 16u) n = 16u;
        p_vkGetPhysicalDeviceQueueFamilyProperties(list[i], &n, qf);
        for (f = 0; f < n; ++f) if (qf[f].queueCount && (qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { g_physical = list[i]; g_family = f; props = q; break; }
    }
    if (!g_physical) { status = 6; first_failure = "eligible adapter"; goto cleanup; }
    LOG("SELECTED: %s vendor=0x%04X family=%u maxImageDimension2D=%u\n", props.deviceName, props.vendorID, g_family, props.limits.maxImageDimension2D);
    p_vkGetPhysicalDeviceMemoryProperties(g_physical, &g_mp);
    for (i = 0; i < (int)g_mp.memoryHeapCount; ++i) LOG("MEMORY_HEAP[%d]: size=%llu flags=0x%X\n", i, (unsigned long long)g_mp.memoryHeaps[i].size, g_mp.memoryHeaps[i].flags);
    {
        float prio = 1.0f; VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0u, g_family, 1u, &prio };
        VkPhysicalDeviceFeatures avail, en; const char *subset = "VK_KHR_portability_subset"; uint32_t n = 0; VkExtensionProperties ep[256];
        VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0u, 1u, &qi, 0u, NULL, 0u, NULL, NULL };
        VkResult r;
        p_vkGetPhysicalDeviceFeatures(g_physical, &avail); memset(&en, 0, sizeof(en));
        if (!avail.textureCompressionBC) { status = 7; first_failure = "textureCompressionBC unavailable"; goto cleanup; }
        en.textureCompressionBC = VK_TRUE; ci.pEnabledFeatures = &en;
        n = 256u;
        if (p_vkEnumerateDeviceExtensionProperties(g_physical, NULL, &n, ep) >= 0)
            for (i = 0; i < (int)n; ++i) if (!strcmp(ep[i].extensionName, subset)) { ci.enabledExtensionCount = 1u; ci.ppEnabledExtensionNames = &subset; }
        r = p_vkCreateDevice(g_physical, &ci, NULL, &g_device);
        LOG("RESULT: vkCreateDevice => %s (%d)\n", rn(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateDevice"; goto cleanup; }
    }
#define LD(n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr(g_device, #n); if (!p_##n) { status = 20; first_failure = #n; goto cleanup; }
    LD(vkDestroyDevice) LD(vkGetDeviceQueue) LD(vkCreateImage) LD(vkDestroyImage) LD(vkGetImageMemoryRequirements) LD(vkAllocateMemory)
    LD(vkFreeMemory) LD(vkBindImageMemory) LD(vkCreateBuffer) LD(vkDestroyBuffer) LD(vkGetBufferMemoryRequirements) LD(vkBindBufferMemory)
    LD(vkMapMemory) LD(vkUnmapMemory) LD(vkInvalidateMappedMemoryRanges) LD(vkFlushMappedMemoryRanges) LD(vkCreateCommandPool) LD(vkDestroyCommandPool) LD(vkAllocateCommandBuffers) LD(vkFreeCommandBuffers)
    LD(vkBeginCommandBuffer) LD(vkEndCommandBuffer) LD(vkCmdPipelineBarrier) LD(vkCmdCopyBufferToImage) LD(vkCmdCopyImageToBuffer)
    LD(vkCmdCopyImage) LD(vkCmdBlitImage) LD(vkCreateFence) LD(vkDestroyFence) LD(vkQueueSubmit) LD(vkWaitForFences) LD(vkDeviceWaitIdle)
#undef LD
    p_vkGetDeviceQueue(g_device, g_family, 0u, &g_queue);
    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_family };
        if (p_vkCreateCommandPool(g_device, &pi, NULL, &g_pool) != VK_SUCCESS) { status = 21; first_failure = "vkCreateCommandPool"; goto cleanup; }
    }
    query_cases(!expect_no_fallback);
    if (expect_no_fallback) no_fallback_cases();
    else {
        if (only && !wcscmp(only, L"control")) control_cases();
        if (!only || !wcscmp(only, L"menu")) menu_background_cases();
        if ((!quick && !only) || (only && !wcscmp(only, L"boundary"))) boundary_cases();
        if ((!quick && !only) || (only && !wcscmp(only, L"srgb"))) srgb_case();
    }
    {
        VkResult r = p_vkDeviceWaitIdle(g_device);
        LOG("RESULT: vkDeviceWaitIdle => %s (%d)\n", rn(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkDeviceWaitIdle"; goto cleanup; }
    }
    if (g_failures) { status = 11; first_failure = "one or more cases failed"; } else status = 0;
cleanup:
    if (g_pool) p_vkDestroyCommandPool(g_device, g_pool, NULL);
    if (g_device && p_vkDestroyDevice) p_vkDestroyDevice(g_device, NULL);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    LOG("SUMMARY: cases=%d failures=%d\n", g_cases, g_failures);
    if (status) ERR("FINAL: FAIL status=%d; FIRST_FAILURE: %s\n", status, first_failure ? first_failure : "unspecified");
    else LOG("FINAL: PASS status=0; %d large-BC7 cases passed (%s).\n", g_cases, expect_no_fallback ? "no-fallback mode" : "decode fallback");
    return status;
}
