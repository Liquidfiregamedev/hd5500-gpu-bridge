/*
 * AtlasBatchProbe v11 -- x64 Windows raw Vulkan atlas-batch diagnostic.
 *
 * This executable deliberately does not link a Vulkan import library.  It uses
 * only vkGetInstanceProcAddr obtained from the caller-supplied loader, and it
 * sets VK_DRIVER_FILES and VK_ICD_FILENAMES before that loader is loaded.
 * API version requested is Vulkan 1.0.
 */
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define INTEL_VENDOR_ID 0x8086u
#define SRC_W 11u
#define SRC_H 9u
#define MAX_TILES 64u
#define SENT_R 0xA1u
#define SENT_G 0xB2u
#define SENT_B 0xC3u
#define SENT_A 0xD4u
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define ERR(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

typedef struct ImageResource { VkImage image; VkDeviceMemory memory; } ImageResource;
typedef struct BufferResource { VkBuffer buffer; VkDeviceMemory memory; VkDeviceSize size; } BufferResource;
typedef struct Tile {
    uint32_t sx, sy, sw, sh;
    uint32_t dx, dy, dw, dh;
    int updated;
} Tile;
typedef struct Config {
    uint32_t atlas_size, tiles;
    int allow_non_intel, multi_region, mip_layer, clone_secondary;
} Config;
typedef struct ExtraInfo {
    ImageResource image;
    VkDeviceSize base;
    VkDeviceSize offsets[4];
    VkBufferImageCopy regions[4];
    VkDeviceSize bytes;
} ExtraInfo;

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkEnumerateInstanceVersion p_vkEnumerateInstanceVersion;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkDestroyInstance p_vkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceFormatProperties p_vkGetPhysicalDeviceFormatProperties;
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;

static PFN_vkDestroyDevice p_vkDestroyDevice;
static PFN_vkGetDeviceQueue p_vkGetDeviceQueue;
static PFN_vkCreateImage p_vkCreateImage;
static PFN_vkDestroyImage p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
static PFN_vkBindImageMemory p_vkBindImageMemory;
static PFN_vkCreateBuffer p_vkCreateBuffer;
static PFN_vkDestroyBuffer p_vkDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindBufferMemory p_vkBindBufferMemory;
static PFN_vkMapMemory p_vkMapMemory;
static PFN_vkUnmapMemory p_vkUnmapMemory;
static PFN_vkFlushMappedMemoryRanges p_vkFlushMappedMemoryRanges;
static PFN_vkInvalidateMappedMemoryRanges p_vkInvalidateMappedMemoryRanges;
static PFN_vkCreateCommandPool p_vkCreateCommandPool;
static PFN_vkDestroyCommandPool p_vkDestroyCommandPool;
static PFN_vkResetCommandPool p_vkResetCommandPool;
static PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
static PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
static PFN_vkCmdCopyBufferToImage p_vkCmdCopyBufferToImage;
static PFN_vkCmdCopyImageToBuffer p_vkCmdCopyImageToBuffer;
static PFN_vkCmdBlitImage p_vkCmdBlitImage;
static PFN_vkCmdExecuteCommands p_vkCmdExecuteCommands;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkDestroyFence p_vkDestroyFence;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkWaitForFences p_vkWaitForFences;
static PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle;

#define LOAD_GLOBAL(n) p_##n = (PFN_##n)gipa(VK_NULL_HANDLE, #n)
#define LOAD_INSTANCE(i,n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d,n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)
#define NEED(n) do { if (!p_##n) { ERR("FAIL: required entry point unavailable: %s\n", #n); status = 20; first_failure = #n " unavailable"; goto cleanup; } } while (0)

static const char *result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default: return "unrecognized VkResult";
    }
}
static int checked(VkResult r, const char *what) {
    LOG("RESULT: %s => %s (%d)\n", what, result_name(r), (int)r);
    return r == VK_SUCCESS;
}
static double qpc_ms(LARGE_INTEGER first, LARGE_INTEGER last, LARGE_INTEGER frequency) {
    return ((double)(last.QuadPart - first.QuadPart) * 1000.0) / (double)frequency.QuadPart;
}
static int absolute_windows_path(const wchar_t *p) {
    return p && ((p[0] == L'\\' && p[1] == L'\\') ||
        (iswalpha((wint_t)p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')));
}
static int ci_contains(const char *s, const char *part) {
    size_t i, n = strlen(part);
    for (; *s; ++s) { for (i = 0; i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)part[i]); ++i) {} if (i == n) return 1; }
    return n == 0;
}
static int software_like(const VkPhysicalDeviceProperties *p) {
    return p->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU || ci_contains(p->deviceName, "warp") ||
        ci_contains(p->deviceName, "swiftshader") || ci_contains(p->deviceName, "lavapipe") ||
        ci_contains(p->deviceName, "llvmpipe") || ci_contains(p->deviceName, "software") || ci_contains(p->deviceName, "basic render");
}
static const char *type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "DISCRETE_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "VIRTUAL_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
    default: return "OTHER";
    }
}
static int instance_extension_present(const char *wanted) {
    uint32_t n = 0, i; VkExtensionProperties *p = NULL; int found = 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS) return 0;
    if (n && !(p = (VkExtensionProperties *)calloc(n, sizeof(*p)))) return 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &n, p) == VK_SUCCESS)
        for (i = 0; i < n; ++i) if (!strcmp(p[i].extensionName, wanted)) { found = 1; break; }
    free(p); return found;
}
static int device_extension_present(VkPhysicalDevice physical, const char *wanted) {
    uint32_t n = 0, i; VkExtensionProperties *p = NULL; int found = 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &n, NULL) != VK_SUCCESS) return 0;
    if (n && !(p = (VkExtensionProperties *)calloc(n, sizeof(*p)))) return 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &n, p) == VK_SUCCESS)
        for (i = 0; i < n; ++i) if (!strcmp(p[i].extensionName, wanted)) { found = 1; break; }
    free(p); return found;
}
static int find_transfer_queue(VkPhysicalDevice physical, uint32_t *out) {
    uint32_t n = 0, i; VkQueueFamilyProperties *q;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, NULL);
    if (!n || !(q = (VkQueueFamilyProperties *)calloc(n, sizeof(*q)))) return 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, q);
    for (i = 0; i < n; ++i) {
        LOG("QUEUE[%u]: flags=0x%X count=%u\n", i, q[i].queueFlags, q[i].queueCount);
        if (q[i].queueCount && (q[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) { *out = i; free(q); return 1; }
    }
    free(q); return 0;
}
static int choose_memory(uint32_t bits, const VkPhysicalDeviceMemoryProperties *mp, VkMemoryPropertyFlags need) {
    uint32_t i;
    for (i = 0; i < mp->memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & need) == need) return (int)i;
    return -1;
}
static void destroy_image(VkDevice d, ImageResource *r) {
    if (r->image) p_vkDestroyImage(d, r->image, NULL);
    if (r->memory) p_vkFreeMemory(d, r->memory, NULL);
    r->image = VK_NULL_HANDLE; r->memory = VK_NULL_HANDLE;
}
static void destroy_buffer(VkDevice d, BufferResource *r) {
    if (r->buffer) p_vkDestroyBuffer(d, r->buffer, NULL);
    if (r->memory) p_vkFreeMemory(d, r->memory, NULL);
    r->buffer = VK_NULL_HANDLE; r->memory = VK_NULL_HANDLE; r->size = 0;
}
static int make_image(VkDevice d, const VkPhysicalDeviceMemoryProperties *mp, uint32_t w, uint32_t h,
                      uint32_t mips, uint32_t layers, VkImageUsageFlags usage, const char *name, ImageResource *out) {
    VkImageCreateInfo ci; VkMemoryRequirements req; VkMemoryAllocateInfo ai; int mt;
    memset(out, 0, sizeof(*out)); memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1; ci.mipLevels = mips; ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    LOG("CALL: vkCreateImage(%s, RGBA8, %ux%u mips=%u layers=%u usage=0x%X)\n", name, w, h, mips, layers, usage);
    if (!checked(p_vkCreateImage(d, &ci, NULL, &out->image), "vkCreateImage")) return 0;
    p_vkGetImageMemoryRequirements(d, out->image, &req);
    mt = choose_memory(req.memoryTypeBits, mp, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    LOG("IMAGE_REQUIREMENTS: %s bytes=%llu alignment=%llu typeBits=0x%08X deviceLocalType=%d\n", name,
        (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits, mt);
    if (mt < 0) { ERR("FAIL: %s lacks compatible DEVICE_LOCAL image memory; no fallback substituted.\n", name); return 0; }
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)mt;
    LOG("CALL: vkAllocateMemory(%s, DEVICE_LOCAL)\n", name);
    if (!checked(p_vkAllocateMemory(d, &ai, NULL, &out->memory), "vkAllocateMemory(image)")) return 0;
    LOG("CALL: vkBindImageMemory(%s)\n", name);
    if (!checked(p_vkBindImageMemory(d, out->image, out->memory, 0), "vkBindImageMemory")) return 0;
    return 1;
}
static int make_host_buffer(VkDevice d, const VkPhysicalDeviceMemoryProperties *mp, VkDeviceSize bytes,
                            VkBufferUsageFlags usage, const char *name, BufferResource *out) {
    VkBufferCreateInfo ci; VkMemoryRequirements req; VkMemoryAllocateInfo ai; int mt;
    memset(out, 0, sizeof(*out)); memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; ci.size = bytes; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    LOG("CALL: vkCreateBuffer(%s, bytes=%llu usage=0x%X)\n", name, (unsigned long long)bytes, usage);
    if (!checked(p_vkCreateBuffer(d, &ci, NULL, &out->buffer), "vkCreateBuffer")) return 0;
    p_vkGetBufferMemoryRequirements(d, out->buffer, &req);
    mt = choose_memory(req.memoryTypeBits, mp, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    LOG("BUFFER_REQUIREMENTS: %s bytes=%llu alignment=%llu typeBits=0x%08X hostCoherentType=%d\n", name,
        (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits, mt);
    if (mt < 0) { ERR("FAIL: %s lacks HOST_VISIBLE|HOST_COHERENT memory.\n", name); return 0; }
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)mt;
    LOG("CALL: vkAllocateMemory(%s, HOST_VISIBLE|HOST_COHERENT)\n", name);
    if (!checked(p_vkAllocateMemory(d, &ai, NULL, &out->memory), "vkAllocateMemory(buffer)")) return 0;
    LOG("CALL: vkBindBufferMemory(%s)\n", name);
    if (!checked(p_vkBindBufferMemory(d, out->buffer, out->memory, 0), "vkBindBufferMemory")) return 0;
    out->size = req.size; return 1;
}
static VkImageSubresourceLayers layers_at(uint32_t mip, uint32_t layer) {
    VkImageSubresourceLayers x; memset(&x, 0, sizeof(x));
    x.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; x.mipLevel = mip; x.baseArrayLayer = layer; x.layerCount = 1;
    return x;
}
static VkImageSubresourceRange range_at(uint32_t base_mip, uint32_t levels, uint32_t base_layer, uint32_t layers) {
    VkImageSubresourceRange x; memset(&x, 0, sizeof(x));
    x.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; x.baseMipLevel = base_mip; x.levelCount = levels; x.baseArrayLayer = base_layer; x.layerCount = layers;
    return x;
}
static void transition(VkCommandBuffer cb, VkImage image, VkImageLayout oldl, VkImageLayout newl,
                       VkAccessFlags src, VkAccessFlags dst, VkImageSubresourceRange range, const char *label) {
    VkImageMemoryBarrier b; VkPipelineStageFlags ss = oldl == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    memset(&b, 0, sizeof(b)); b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldl; b.newLayout = newl; b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = src; b.dstAccessMask = dst; b.image = image; b.subresourceRange = range;
    LOG("CALL: vkCmdPipelineBarrier(%s)\n", label);
    p_vkCmdPipelineBarrier(cb, ss, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    LOG("RESULT: vkCmdPipelineBarrier(%s) => recorded\n", label);
}
static void host_barrier(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize bytes, const char *label) {
    VkBufferMemoryBarrier b; memset(&b, 0, sizeof(b)); b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.buffer = buffer; b.offset = offset; b.size = bytes;
    LOG("CALL: vkCmdPipelineBarrier(%s)\n", label);
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &b, 0, NULL);
    LOG("RESULT: vkCmdPipelineBarrier(%s) => recorded\n", label);
}
static void source_pixel(uint32_t x, uint32_t y, uint8_t rgba[4]) {
    /* Each of the 99 texels has a distinctive, non-sentinel RGBA code. */
    rgba[0] = (uint8_t)(0x13u + x * 19u + y * 7u);
    rgba[1] = (uint8_t)(0xE7u - x * 11u - y * 17u);
    rgba[2] = (uint8_t)(0x31u + x * 23u + y * 29u);
    rgba[3] = (uint8_t)(0x41u + x * 9u + y * 13u);
}
static void sentinel_pixel(uint8_t rgba[4]) { rgba[0]=SENT_R; rgba[1]=SENT_G; rgba[2]=SENT_B; rgba[3]=SENT_A; }
static void extra_sentinel(uint32_t which, uint8_t rgba[4]) {
    rgba[0] = (uint8_t)(0x60u + which * 13u); rgba[1] = (uint8_t)(0x20u + which * 29u);
    rgba[2] = (uint8_t)(0xD0u - which * 17u); rgba[3] = (uint8_t)(0x80u + which * 7u);
}
static uint32_t nearest_coord(uint32_t local, uint32_t src_extent, uint32_t dst_extent) {
    uint32_t v = (uint32_t)(((uint64_t)(2u * local + 1u) * src_extent) / (2u * dst_extent));
    return v < src_extent ? v : src_extent - 1u;
}
static void tile_pixel(const Tile *t, uint32_t x, uint32_t y, uint8_t rgba[4]) {
    uint32_t sx = t->sx + nearest_coord(x - t->dx, t->sw, t->dw);
    uint32_t sy = t->sy + nearest_coord(y - t->dy, t->sh, t->dh);
    source_pixel(sx, sy, rgba);
}
static int point_in_tile(const Tile *t, uint32_t x, uint32_t y) {
    return x >= t->dx && x < t->dx + t->dw && y >= t->dy && y < t->dy + t->dh;
}
static void expected_atlas_pixel(const Tile *tiles, uint32_t ntile, uint32_t x, uint32_t y, uint8_t rgba[4]) {
    uint32_t i; sentinel_pixel(rgba);
    for (i = 0; i < ntile; ++i) if (point_in_tile(&tiles[i], x, y)) { tile_pixel(&tiles[i], x, y, rgba); return; }
}
static void build_tiles(Tile *tiles, uint32_t count, uint32_t atlas) {
    uint32_t i, cols = 8u, rows = 8u;
    for (i = 0; i < count; ++i) {
        uint32_t slot = i >= 4u ? i - 4u : 0u;
        uint32_t col = slot % cols, row = slot / cols;
        uint32_t dw = (i & 1u) ? 15u : 13u, dh = (i & 2u) ? 11u : 9u;
        uint32_t xmax = atlas - dw - 32u, ymax = atlas - dh - 32u;
        /* The uploaded source image is 11x9; active 9x7 rectangles guarantee
           no 0.5 nearest-sampling tie against the required odd tile extents. */
        tiles[i].sx = 1; tiles[i].sy = 1; tiles[i].sw = 9; tiles[i].sh = 7;
        tiles[i].dw = dw; tiles[i].dh = dh;
        /* Interior slots exclude all four explicit corners, even at 64 tiles. */
        tiles[i].dx = 32u + (uint32_t)(((uint64_t)col * (xmax - 32u)) / (cols - 1u));
        tiles[i].dy = 32u + (uint32_t)(((uint64_t)row * (ymax - 32u)) / (rows - 1u));
        tiles[i].updated = 0;
    }
    /* Explicit corner and very large nonzero placements, independent of grid rounding. */
    if (count > 0) { tiles[0].dx = 1; tiles[0].dy = 1; }
    if (count > 1) { tiles[1].dx = atlas - tiles[1].dw - 1u; tiles[1].dy = 1; }
    if (count > 2) { tiles[2].dx = 1; tiles[2].dy = atlas - tiles[2].dh - 1u; }
    if (count > 3) { tiles[3].dx = atlas - tiles[3].dw - 1u; tiles[3].dy = atlas - tiles[3].dh - 1u; }
}
static int check_nonoverlap(const Tile *tiles, uint32_t n, uint32_t atlas) {
    uint32_t i, j;
    for (i = 0; i < n; ++i) {
        if (!tiles[i].dw || !tiles[i].dh || tiles[i].dx + tiles[i].dw > atlas || tiles[i].dy + tiles[i].dh > atlas) return 0;
        for (j = 0; j < i; ++j)
            if (!(tiles[i].dx + tiles[i].dw <= tiles[j].dx || tiles[j].dx + tiles[j].dw <= tiles[i].dx ||
                  tiles[i].dy + tiles[i].dh <= tiles[j].dy || tiles[j].dy + tiles[j].dh <= tiles[i].dy)) return 0;
    }
    return 1;
}
static VkDeviceSize extra_setup(ExtraInfo *e, VkDeviceSize base) {
    uint32_t i; VkDeviceSize at = base;
    static const uint32_t widths[4] = {64u,32u,64u,32u};
    static const uint32_t heights[4] = {48u,24u,48u,24u};
    static const uint32_t mips[4] = {0u,1u,0u,1u};
    static const uint32_t layers[4] = {0u,0u,1u,1u};
    memset(e, 0, sizeof(*e)); e->base = base;
    for (i = 0; i < 4; ++i) {
        e->offsets[i] = at; memset(&e->regions[i], 0, sizeof(e->regions[i]));
        e->regions[i].bufferOffset = at; e->regions[i].imageSubresource = layers_at(mips[i], layers[i]);
        e->regions[i].imageExtent.width = widths[i]; e->regions[i].imageExtent.height = heights[i]; e->regions[i].imageExtent.depth = 1;
        at += (VkDeviceSize)widths[i] * heights[i] * 4u;
    }
    e->bytes = at - base; return at;
}
static int fill_upload(VkDevice d, BufferResource *upload, uint32_t atlas, VkDeviceSize atlas_bytes,
                       VkDeviceSize source_base, const ExtraInfo *extra, int want_extra) {
    uint8_t *p, rgba[4]; VkMappedMemoryRange range; uint32_t x, y, i;
    LOG("CALL: vkMapMemory(upload staging full atlas)\n");
    if (!checked(p_vkMapMemory(d, upload->memory, 0, upload->size, 0, (void **)&p), "vkMapMemory(upload)")) return 0;
    for (y = 0; y < atlas; ++y) for (x = 0; x < atlas; ++x) {
        uint8_t *q = p + ((VkDeviceSize)y * atlas + x) * 4u; sentinel_pixel(rgba); memcpy(q, rgba, 4);
    }
    for (y = 0; y < SRC_H; ++y) for (x = 0; x < SRC_W; ++x) { source_pixel(x, y, rgba); memcpy(p + source_base + (y * SRC_W + x) * 4u, rgba, 4); }
    if (want_extra) {
        for (i = 0; i < 4; ++i) {
            uint32_t w = extra->regions[i].imageExtent.width, h = extra->regions[i].imageExtent.height;
            extra_sentinel(i, rgba);
            for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) memcpy(p + extra->offsets[i] + ((VkDeviceSize)y * w + x) * 4u, rgba, 4);
        }
    }
    memset(&range, 0, sizeof(range)); range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = upload->memory; range.offset = 0; range.size = VK_WHOLE_SIZE;
    LOG("CALL: vkFlushMappedMemoryRanges(upload staging)\n");
    if (!checked(p_vkFlushMappedMemoryRanges(d, 1, &range), "vkFlushMappedMemoryRanges(upload)")) { p_vkUnmapMemory(d, upload->memory); return 0; }
    p_vkUnmapMemory(d, upload->memory);
    LOG("UPLOAD CONTENT: real full %ux%u RGBA8 sentinel upload (%llu bytes), plus distinctive 11x9 source pixels%s.\n",
        atlas, atlas, (unsigned long long)atlas_bytes, want_extra ? " and 2-mip/2-layer neighbor image" : "");
    return 1;
}
static void blit_tile(VkCommandBuffer cb, VkImage src, VkImage dst, const Tile *t, uint32_t dst_mip, uint32_t dst_layer, const char *label) {
    VkImageBlit b; memset(&b, 0, sizeof(b)); b.srcSubresource = layers_at(0, 0); b.dstSubresource = layers_at(dst_mip, dst_layer);
    b.srcOffsets[0].x = (int32_t)t->sx; b.srcOffsets[0].y = (int32_t)t->sy; b.srcOffsets[0].z = 0;
    b.srcOffsets[1].x = (int32_t)(t->sx + t->sw); b.srcOffsets[1].y = (int32_t)(t->sy + t->sh); b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = (int32_t)t->dx; b.dstOffsets[0].y = (int32_t)t->dy; b.dstOffsets[0].z = 0;
    b.dstOffsets[1].x = (int32_t)(t->dx + t->dw); b.dstOffsets[1].y = (int32_t)(t->dy + t->dh); b.dstOffsets[1].z = 1;
    LOG("CALL: vkCmdBlitImage(%s src=(%u,%u %ux%u) dst=(%u,%u %ux%u) NEAREST)\n", label,
        t->sx,t->sy,t->sw,t->sh,t->dx,t->dy,t->dw,t->dh);
    p_vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    LOG("RESULT: vkCmdBlitImage(%s) => recorded\n", label);
}
static void blit_three_regions(VkCommandBuffer cb, VkImage src, VkImage dst, const Tile *tiles) {
    VkImageBlit b[3]; uint32_t i;
    memset(b, 0, sizeof(b));
    for (i = 0; i < 3; ++i) {
        b[i].srcSubresource = layers_at(0, 0); b[i].dstSubresource = layers_at(0, 0);
        b[i].srcOffsets[0].x=(int32_t)tiles[i].sx; b[i].srcOffsets[0].y=(int32_t)tiles[i].sy;
        b[i].srcOffsets[1].x=(int32_t)(tiles[i].sx+tiles[i].sw); b[i].srcOffsets[1].y=(int32_t)(tiles[i].sy+tiles[i].sh); b[i].srcOffsets[1].z=1;
        b[i].dstOffsets[0].x=(int32_t)tiles[i].dx; b[i].dstOffsets[0].y=(int32_t)tiles[i].dy;
        b[i].dstOffsets[1].x=(int32_t)(tiles[i].dx+tiles[i].dw); b[i].dstOffsets[1].y=(int32_t)(tiles[i].dy+tiles[i].dh); b[i].dstOffsets[1].z=1;
    }
    LOG("CALL: vkCmdBlitImage(MULTI_REGION same original atlas destination, 3 disjoint nonzero/corner regions, NEAREST)\n");
    p_vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 3, b, VK_FILTER_NEAREST);
    LOG("RESULT: vkCmdBlitImage(MULTI_REGION) => recorded; validates local coordinates rather than destination bounding box.\n");
}
static void copy_full_atlas_to_buffer(VkCommandBuffer cb, VkImage atlas, VkBuffer rb, uint32_t size, VkDeviceSize offset, const char *label) {
    VkBufferImageCopy r; memset(&r, 0, sizeof(r)); r.bufferOffset=offset; r.imageSubresource=layers_at(0,0); r.imageExtent.width=size; r.imageExtent.height=size; r.imageExtent.depth=1;
    LOG("CALL: vkCmdCopyImageToBuffer(%s, complete %ux%u atlas)\n", label, size, size);
    p_vkCmdCopyImageToBuffer(cb, atlas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &r);
    LOG("RESULT: vkCmdCopyImageToBuffer(%s) => recorded\n", label);
}
static int begin_work(VkCommandBuffer worker, VkCommandBuffer primary, int clone) {
    VkCommandBufferBeginInfo bi; VkCommandBufferInheritanceInfo inh;
    memset(&bi,0,sizeof(bi)); memset(&inh,0,sizeof(inh)); bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    inh.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO; if (clone) bi.pInheritanceInfo=&inh;
    LOG("CALL: vkBeginCommandBuffer(%s worker)\n", clone ? "secondary" : "primary");
    if (!checked(p_vkBeginCommandBuffer(worker,&bi),"vkBeginCommandBuffer(worker)")) return 0;
    (void)primary; return 1;
}
static int end_work(VkCommandBuffer worker, VkCommandBuffer primary, int clone) {
    VkCommandBufferBeginInfo bi;
    LOG("CALL: vkEndCommandBuffer(worker)\n");
    if (!checked(p_vkEndCommandBuffer(worker),"vkEndCommandBuffer(worker)")) return 0;
    if (!clone) return 1;
    memset(&bi,0,sizeof(bi)); bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    LOG("CALL: vkBeginCommandBuffer(primary execute wrapper)\n");
    if (!checked(p_vkBeginCommandBuffer(primary,&bi),"vkBeginCommandBuffer(primary)")) return 0;
    LOG("CALL: vkCmdExecuteCommands(real recorded secondary)\n"); p_vkCmdExecuteCommands(primary,1,&worker);
    LOG("RESULT: vkCmdExecuteCommands => recorded\n");
    LOG("CALL: vkEndCommandBuffer(primary execute wrapper)\n");
    return checked(p_vkEndCommandBuffer(primary),"vkEndCommandBuffer(primary)");
}
static int record_first(VkCommandBuffer worker, VkCommandBuffer primary, int clone, VkImage source, VkImage atlas,
                        BufferResource *up, BufferResource *rb, uint32_t size, VkDeviceSize source_base,
                        Tile *tiles, uint32_t ntile, int multi, ExtraInfo *extra, int mip_layer) {
    VkBufferImageCopy src_copy, atlas_copy; VkImageSubresourceRange one=range_at(0,1,0,1); uint32_t i;
    if (!begin_work(worker,primary,clone)) return 0;
    transition(worker, source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,VK_ACCESS_TRANSFER_WRITE_BIT,one,"source UNDEFINED->TRANSFER_DST");
    transition(worker, atlas, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,VK_ACCESS_TRANSFER_WRITE_BIT,one,"original atlas UNDEFINED->TRANSFER_DST");
    memset(&atlas_copy,0,sizeof(atlas_copy)); atlas_copy.imageSubresource=layers_at(0,0); atlas_copy.imageExtent.width=size; atlas_copy.imageExtent.height=size; atlas_copy.imageExtent.depth=1;
    LOG("CALL: vkCmdCopyBufferToImage(real full atlas sentinel upload)\n"); p_vkCmdCopyBufferToImage(worker,up->buffer,atlas,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&atlas_copy); LOG("RESULT: vkCmdCopyBufferToImage(full atlas) => recorded\n");
    memset(&src_copy,0,sizeof(src_copy)); src_copy.bufferOffset=source_base; src_copy.imageSubresource=layers_at(0,0); src_copy.imageExtent.width=SRC_W; src_copy.imageExtent.height=SRC_H; src_copy.imageExtent.depth=1;
    LOG("CALL: vkCmdCopyBufferToImage(distinctive 11x9 source upload)\n"); p_vkCmdCopyBufferToImage(worker,up->buffer,source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&src_copy); LOG("RESULT: vkCmdCopyBufferToImage(source) => recorded\n");
    transition(worker,source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,one,"source TRANSFER_DST->TRANSFER_SRC");
    transition(worker,atlas,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,one,"atlas upload write->blit writes");
    if (multi) { blit_three_regions(worker,source,atlas,tiles); for (i=3;i<ntile;++i) blit_tile(worker,source,atlas,&tiles[i],0,0,"batch individual"); }
    else for (i=0;i<ntile;++i) blit_tile(worker,source,atlas,&tiles[i],0,0,"batch individual");
    if (mip_layer) {
        VkImageSubresourceRange all=range_at(0,2,0,2); Tile mt;
        transition(worker,extra->image.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,all,"mip-layer image UNDEFINED->TRANSFER_DST");
        LOG("CALL: vkCmdCopyBufferToImage(mip-layer all 4 selected/neighbor subresources)\n"); p_vkCmdCopyBufferToImage(worker,up->buffer,extra->image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,4,extra->regions); LOG("RESULT: vkCmdCopyBufferToImage(mip-layer) => recorded\n");
        transition(worker,extra->image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,all,"mip-layer upload write->blit writes");
        memset(&mt,0,sizeof(mt)); mt.sx=1; mt.sy=1; mt.sw=9; mt.sh=7; mt.dx=5; mt.dy=4; mt.dw=13; mt.dh=11;
        blit_tile(worker,source,extra->image.image,&mt,1,1,"mip=1 layer=1 nonzero origin");
        transition(worker,extra->image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,all,"mip-layer all subresources TRANSFER_DST->TRANSFER_SRC");
        LOG("CALL: vkCmdCopyImageToBuffer(mip-layer selected mip/layer plus all neighbors)\n"); p_vkCmdCopyImageToBuffer(worker,extra->image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,rb->buffer,4,extra->regions); LOG("RESULT: vkCmdCopyImageToBuffer(mip-layer all) => recorded\n");
        host_barrier(worker,rb->buffer,extra->base,extra->bytes,"mip-layer readback TRANSFER_WRITE->HOST_READ");
    }
    transition(worker,atlas,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,one,"atlas blits TRANSFER_DST->TRANSFER_SRC");
    copy_full_atlas_to_buffer(worker,atlas,rb->buffer,size,0,"first atlas exact readback");
    host_barrier(worker,rb->buffer,0,(VkDeviceSize)size*size*4u,"first atlas readback TRANSFER_WRITE->HOST_READ");
    return end_work(worker,primary,clone);
}
static int record_second(VkCommandBuffer worker, VkCommandBuffer primary, int clone, VkImage source, VkImage atlas,
                         BufferResource *rb, uint32_t size, Tile *tiles, uint32_t ntile) {
    VkImageSubresourceRange one=range_at(0,1,0,1); uint32_t i, updated=0;
    if (!begin_work(worker,primary,clone)) return 0;
    transition(worker,atlas,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,one,"second batch atlas TRANSFER_SRC->TRANSFER_DST");
    for (i=0;i<ntile;i+=2) {
        /* A distinct 9x7 source payload also avoids nearest half ties. */
        tiles[i].sx=0; tiles[i].sy=0; tiles[i].sw=9; tiles[i].sh=7; tiles[i].updated=1;
        blit_tile(worker,source,atlas,&tiles[i],0,0,"second-batch update"); ++updated;
    }
    LOG("BATCH SECOND: recorded %u real individual update blits; all other first-batch tiles must remain unchanged.\n",updated);
    transition(worker,atlas,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,one,"second batch atlas TRANSFER_DST->TRANSFER_SRC");
    copy_full_atlas_to_buffer(worker,atlas,rb->buffer,size,0,"second atlas exact readback");
    host_barrier(worker,rb->buffer,0,(VkDeviceSize)size*size*4u,"second atlas readback TRANSFER_WRITE->HOST_READ");
    return end_work(worker,primary,clone);
}
static int submit_wait(VkDevice d, VkQueue q, VkCommandBuffer cb) {
    VkFenceCreateInfo fi; VkFence fence=VK_NULL_HANDLE; VkSubmitInfo si; int ok=0;
    memset(&fi,0,sizeof(fi)); fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; LOG("CALL: vkCreateFence(batch)\n");
    if (!checked(p_vkCreateFence(d,&fi,NULL,&fence),"vkCreateFence")) goto out;
    memset(&si,0,sizeof(si)); si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&cb;
    LOG("CALL: vkQueueSubmit(batch)\n"); if (!checked(p_vkQueueSubmit(q,1,&si,fence),"vkQueueSubmit")) goto out;
    LOG("CALL: vkWaitForFences(batch,30s)\n"); if (!checked(p_vkWaitForFences(d,1,&fence,VK_TRUE,UINT64_C(30000000000)),"vkWaitForFences")) goto out;
    ok=1;
out: if(fence) p_vkDestroyFence(d,fence,NULL); return ok;
}
static int verify_atlas(VkDevice d, BufferResource *rb, const Tile *tiles, uint32_t ntile, uint32_t size, const char *phase) {
    uint8_t *p, expected[4]; VkMappedMemoryRange range; uint32_t x,y;
    LOG("CALL: vkMapMemory(readback complete atlas %s)\n",phase);
    if (!checked(p_vkMapMemory(d,rb->memory,0,rb->size,0,(void **)&p),"vkMapMemory(readback)")) return 0;
    memset(&range,0,sizeof(range)); range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory=rb->memory; range.offset=0; range.size=VK_WHOLE_SIZE;
    LOG("CALL: vkInvalidateMappedMemoryRanges(readback %s)\n",phase);
    if (!checked(p_vkInvalidateMappedMemoryRanges(d,1,&range),"vkInvalidateMappedMemoryRanges(readback)")) { p_vkUnmapMemory(d,rb->memory); return 0; }
    for (y=0;y<size;++y) for (x=0;x<size;++x) {
        const uint8_t *actual=p+((VkDeviceSize)y*size+x)*4u; expected_atlas_pixel(tiles,ntile,x,y,expected);
        if (memcmp(actual,expected,4)) { ERR("FAIL: ATLAS_%s first bad coordinate=(%u,%u) byteOffset=%llu actual=%02X%02X%02X%02X expected=%02X%02X%02X%02X (verification stops after first bounded coordinate).\n",phase,x,y,(unsigned long long)(((VkDeviceSize)y*size+x)*4u),actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]); p_vkUnmapMemory(d,rb->memory); return 0; }
    }
    p_vkUnmapMemory(d,rb->memory); return 1;
}
static int verify_extra(VkDevice d, BufferResource *rb, const ExtraInfo *extra) {
    uint8_t *p, expected[4]; VkMappedMemoryRange range; uint32_t i,x,y;
    Tile mt;
    memset(&mt,0,sizeof(mt)); mt.sx=1;mt.sy=1;mt.sw=9;mt.sh=7;mt.dx=5;mt.dy=4;mt.dw=13;mt.dh=11;
    LOG("CALL: vkMapMemory(readback mip/layer)\n"); if (!checked(p_vkMapMemory(d,rb->memory,0,rb->size,0,(void **)&p),"vkMapMemory(mip-layer readback)")) return 0;
    memset(&range,0,sizeof(range)); range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=rb->memory;range.offset=0;range.size=VK_WHOLE_SIZE;
    LOG("CALL: vkInvalidateMappedMemoryRanges(mip/layer readback)\n"); if (!checked(p_vkInvalidateMappedMemoryRanges(d,1,&range),"vkInvalidateMappedMemoryRanges(mip-layer)")) {p_vkUnmapMemory(d,rb->memory);return 0;}
    for(i=0;i<4;++i) {
        uint32_t w=extra->regions[i].imageExtent.width,h=extra->regions[i].imageExtent.height;
        for(y=0;y<h;++y) for(x=0;x<w;++x) {
            const uint8_t *actual=p+extra->offsets[i]+((VkDeviceSize)y*w+x)*4u;
            if(i==3 && point_in_tile(&mt,x,y)) tile_pixel(&mt,x,y,expected); else extra_sentinel(i,expected);
            if(memcmp(actual,expected,4)) { ERR("FAIL: MIP_LAYER first bad subresource=(mip%u,layer%u) coordinate=(%u,%u) actual=%02X%02X%02X%02X expected=%02X%02X%02X%02X.\n",extra->regions[i].imageSubresource.mipLevel,extra->regions[i].imageSubresource.baseArrayLayer,x,y,actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);p_vkUnmapMemory(d,rb->memory);return 0; }
        }
    }
    p_vkUnmapMemory(d,rb->memory); return 1;
}
static void usage(const wchar_t *exe) {
    LOG("Usage:\n  %ls --loader C:\\absolute\\vulkan-1.dll --icd C:\\absolute\\bridge_icd.json [options]\n",exe);
    LOG("Options: --atlas-size N (512..4096; default 4096), --tiles N (1..64; default 32),\n");
    LOG("         --multi-region (default/idempotent), --mip-layer, --clone-secondary, --allow-non-intel-test\n");
    LOG("Default selection is a non-software Intel adapter. --allow-non-intel-test is sandbox-only and never validates HD 5500.\n");
}
int wmain(int argc, wchar_t **argv) {
    Config cfg; const wchar_t *loader_path=NULL,*icd_path=NULL; HMODULE loader=NULL; union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } proc;
    VkInstance instance=VK_NULL_HANDLE; VkPhysicalDevice *adapters=NULL,physical=VK_NULL_HANDLE; VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mp;
    uint32_t adapter_count=0,family=UINT32_MAX,i; VkDevice device=VK_NULL_HANDLE; VkQueue queue=VK_NULL_HANDLE;
    ImageResource source={0},atlas={0}; ExtraInfo extra; BufferResource upload={0},readback={0}; VkCommandPool pool=VK_NULL_HANDLE; VkCommandBuffer worker=VK_NULL_HANDLE,primary=VK_NULL_HANDLE,submit_cb=VK_NULL_HANDLE;
    Tile tiles[MAX_TILES]; VkDeviceSize atlas_bytes,source_base,total_bytes; int status=1; const char *first_failure=NULL;
    LARGE_INTEGER qpc_frequency, timing_start, timing_recorded, timing_submitted, timing_verified;
    memset(&cfg,0,sizeof(cfg));cfg.atlas_size=4096;cfg.tiles=32;cfg.multi_region=1; memset(&props,0,sizeof(props));memset(&mp,0,sizeof(mp));memset(&extra,0,sizeof(extra));
    for(i=1;i<(uint32_t)argc;++i) {
        if(!wcscmp(argv[i],L"--loader") && i+1<(uint32_t)argc) loader_path=argv[++i];
        else if(!wcscmp(argv[i],L"--icd") && i+1<(uint32_t)argc) icd_path=argv[++i];
        else if(!wcscmp(argv[i],L"--allow-non-intel-test")) cfg.allow_non_intel=1;
        else if(!wcscmp(argv[i],L"--multi-region")) cfg.multi_region=1;
        else if(!wcscmp(argv[i],L"--mip-layer")) cfg.mip_layer=1;
        else if(!wcscmp(argv[i],L"--clone-secondary")) cfg.clone_secondary=1;
        else if(!wcscmp(argv[i],L"--atlas-size") && i+1<(uint32_t)argc) { wchar_t *end=NULL; unsigned long n=wcstoul(argv[++i],&end,10); if(!end||*end||n<512||n>4096) {ERR("FAIL: --atlas-size must be an integer in 512..4096.\n");return 2;} cfg.atlas_size=(uint32_t)n; }
        else if(!wcscmp(argv[i],L"--tiles") && i+1<(uint32_t)argc) { wchar_t *end=NULL; unsigned long n=wcstoul(argv[++i],&end,10); if(!end||*end||n<1||n>MAX_TILES) {ERR("FAIL: --tiles must be 1..64.\n");return 2;}cfg.tiles=(uint32_t)n; }
        else if(!wcscmp(argv[i],L"--help")||!wcscmp(argv[i],L"-h")){usage(argv[0]);return 0;}
        else {ERR("FAIL: unrecognized or incomplete argument: %ls\n",argv[i]);usage(argv[0]);return 2;}
    }
    if(cfg.multi_region && cfg.tiles<3) {ERR("FAIL: --multi-region needs --tiles >= 3.\n");return 2;}
    if(!loader_path||!icd_path||!absolute_windows_path(loader_path)||!absolute_windows_path(icd_path)){ERR("FAIL: mandatory --loader and --icd must be absolute Windows paths.\n");usage(argv[0]);return 2;}
    if(GetFileAttributesW(loader_path)==INVALID_FILE_ATTRIBUTES||GetFileAttributesW(icd_path)==INVALID_FILE_ATTRIBUTES){ERR("FAIL: supplied loader or ICD path does not exist (GetLastError=%lu).\n",GetLastError());return 2;}
    if(!QueryPerformanceFrequency(&qpc_frequency) || !qpc_frequency.QuadPart){ERR("FAIL: QueryPerformanceFrequency unavailable; cannot emit measured diagnostic timings.\n");return 3;}
    if(GetModuleHandleW(L"vulkan-1.dll")){ERR("FAIL: vulkan-1.dll was already loaded; refusing ambiguous supplied-loader isolation.\n");return 3;}
    if(!SetEnvironmentVariableW(L"VK_DRIVER_FILES",icd_path)||!SetEnvironmentVariableW(L"VK_ICD_FILENAMES",icd_path)){ERR("FAIL: cannot set VK_DRIVER_FILES and VK_ICD_FILENAMES before instance creation (GetLastError=%lu).\n",GetLastError());return 3;}
    atlas_bytes=(VkDeviceSize)cfg.atlas_size*cfg.atlas_size*4u; source_base=atlas_bytes; total_bytes=source_base+SRC_W*SRC_H*4u;
    if(cfg.mip_layer) total_bytes=extra_setup(&extra,total_bytes);
    if(total_bytes>(VkDeviceSize)256u*1024u*1024u){ERR("FAIL: configuration exceeds probe 256MiB individual host buffer cap.\n");return 2;}
    build_tiles(tiles,cfg.tiles,cfg.atlas_size); if(!check_nonoverlap(tiles,cfg.tiles,cfg.atlas_size)){ERR("FAIL: internal tile placement invalid/non-disjoint; refusing fabricated expected result.\n");return 2;}
    LOG("AtlasBatchProbe v11 raw Vulkan standalone\nLOADER: %ls\nICD: %ls\n",loader_path,icd_path);
    LOG("CONFIG: atlas=%ux%u bytes=%llu tiles=%u multi-region=%s mip-layer=%s clone-secondary=%s\n",cfg.atlas_size,cfg.atlas_size,(unsigned long long)atlas_bytes,cfg.tiles,cfg.multi_region?"yes":"no",cfg.mip_layer?"yes":"no",cfg.clone_secondary?"yes":"no");
    LOG("ORIGINAL ATLAS REPRESENTATION: RGBA8_UNORM, usage=TRANSFER_SRC|TRANSFER_DST|SAMPLED (explicitly NO STORAGE).\n");
    if(cfg.allow_non_intel)LOG("WARNING: --allow-non-intel-test is sandbox-only; a non-Intel/software result is NOT HD 5500 validation.\n");
    loader=LoadLibraryW(loader_path);if(!loader){status=4;first_failure="LoadLibraryW(loader)";ERR("FAIL: LoadLibraryW(loader) GetLastError=%lu\n",GetLastError());goto cleanup;}
    proc.raw=GetProcAddress(loader,"vkGetInstanceProcAddr");gipa=proc.typed;if(!gipa){status=4;first_failure="vkGetInstanceProcAddr";ERR("FAIL: loader did not expose vkGetInstanceProcAddr.\n");goto cleanup;}
    LOAD_GLOBAL(vkEnumerateInstanceVersion);LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties);LOAD_GLOBAL(vkCreateInstance);NEED(vkEnumerateInstanceExtensionProperties);NEED(vkCreateInstance);
    if(p_vkEnumerateInstanceVersion){uint32_t v=VK_API_VERSION_1_0;LOG("CALL: vkEnumerateInstanceVersion\n");if(!checked(p_vkEnumerateInstanceVersion(&v),"vkEnumerateInstanceVersion")){status=21;first_failure="vkEnumerateInstanceVersion";goto cleanup;}LOG("LOADER MAX API: %u.%u.%u; application requests 1.0.0\n",VK_VERSION_MAJOR(v),VK_VERSION_MINOR(v),VK_VERSION_PATCH(v));}
    { VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO,NULL,"AtlasBatchProbe v11",11,"standalone",11,VK_API_VERSION_1_0}; VkInstanceCreateInfo ci; const char *ext[1];uint32_t n=0;memset(&ci,0,sizeof(ci));ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pApplicationInfo=&ai;
      if(instance_extension_present(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)){ci.flags|=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;ext[n++]=VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;LOG("PORTABILITY: enabling advertised VK_KHR_portability_enumeration.\n");}ci.enabledExtensionCount=n;ci.ppEnabledExtensionNames=ext;LOG("CALL: vkCreateInstance(api=1.0)\n");if(!checked(p_vkCreateInstance(&ci,NULL,&instance),"vkCreateInstance")){status=21;first_failure="vkCreateInstance";goto cleanup;} }
    LOAD_INSTANCE(instance,vkDestroyInstance);LOAD_INSTANCE(instance,vkEnumeratePhysicalDevices);LOAD_INSTANCE(instance,vkGetPhysicalDeviceProperties);LOAD_INSTANCE(instance,vkGetPhysicalDeviceMemoryProperties);LOAD_INSTANCE(instance,vkGetPhysicalDeviceQueueFamilyProperties);LOAD_INSTANCE(instance,vkGetPhysicalDeviceFormatProperties);LOAD_INSTANCE(instance,vkEnumerateDeviceExtensionProperties);LOAD_INSTANCE(instance,vkCreateDevice);LOAD_INSTANCE(instance,vkGetDeviceProcAddr);
    NEED(vkDestroyInstance);NEED(vkEnumeratePhysicalDevices);NEED(vkGetPhysicalDeviceProperties);NEED(vkGetPhysicalDeviceMemoryProperties);NEED(vkGetPhysicalDeviceQueueFamilyProperties);NEED(vkGetPhysicalDeviceFormatProperties);NEED(vkEnumerateDeviceExtensionProperties);NEED(vkCreateDevice);NEED(vkGetDeviceProcAddr);
    LOG("CALL: vkEnumeratePhysicalDevices(count)\n");if(!checked(p_vkEnumeratePhysicalDevices(instance,&adapter_count,NULL),"vkEnumeratePhysicalDevices(count)")){status=21;first_failure="vkEnumeratePhysicalDevices";goto cleanup;}if(!adapter_count){status=5;first_failure="zero physical devices";ERR("FAIL: ICD reports no physical device.\n");goto cleanup;}
    adapters=(VkPhysicalDevice*)calloc(adapter_count,sizeof(*adapters));if(!adapters){status=5;first_failure="adapter list allocation";goto cleanup;}LOG("CALL: vkEnumeratePhysicalDevices(list)\n");if(!checked(p_vkEnumeratePhysicalDevices(instance,&adapter_count,adapters),"vkEnumeratePhysicalDevices(list)")){status=21;first_failure="vkEnumeratePhysicalDevices list";goto cleanup;}
    for(i=0;i<adapter_count;++i){VkPhysicalDeviceProperties p;uint32_t f;p_vkGetPhysicalDeviceProperties(adapters[i],&p);LOG("ADAPTER[%u]: %s vendor=0x%04X type=%s%s\n",i,p.deviceName,p.vendorID,type_name(p.deviceType),software_like(&p)?" [software-like]":"");if(!cfg.allow_non_intel&&(p.vendorID!=INTEL_VENDOR_ID||software_like(&p)))continue;if(!physical&&find_transfer_queue(adapters[i],&f)){physical=adapters[i];family=f;props=p;}}
    if(!physical){status=6;first_failure="eligible non-software Intel adapter";ERR("FAIL: default requires a non-software Intel adapter; --allow-non-intel-test is sandbox-only.\n");goto cleanup;}if(props.vendorID!=INTEL_VENDOR_ID||software_like(&props))LOG("WARNING: selected non-Intel/software-like adapter; test output is sandbox-only.\n");LOG("SELECTED: %s vendor=0x%04X queueFamily=%u\n",props.deviceName,props.vendorID,family);p_vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    {VkFormatProperties fp;VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;p_vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&fp);LOG("FORMAT RGBA8 optimalFeatures=0x%X required=0x%X (no STORAGE requirement)\n",fp.optimalTilingFeatures,needed);if((fp.optimalTilingFeatures&needed)!=needed){status=7;first_failure="RGBA8 transfer/sampled/blit feature gate";ERR("FAIL: RGBA8 format lacks required advertised transfer/sampled/blit features.\n");goto cleanup;}}
    {float priority=1.0f; const char *portability_subset="VK_KHR_portability_subset"; VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,NULL,0,family,1,&priority};VkDeviceCreateInfo di={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,NULL,0,1,&qi,0,NULL,0,NULL,NULL};if(device_extension_present(physical,portability_subset)){di.enabledExtensionCount=1;di.ppEnabledExtensionNames=&portability_subset;LOG("PORTABILITY: enabling advertised VK_KHR_portability_subset.\n");}LOG("CALL: vkCreateDevice(api=1.0 queue)\n");if(!checked(p_vkCreateDevice(physical,&di,NULL,&device),"vkCreateDevice")){status=21;first_failure="vkCreateDevice";goto cleanup;}}
    /* Resolve and validate every device function only after vkGetDeviceProcAddr is known. */
    LOAD_DEVICE(device,vkDestroyDevice);LOAD_DEVICE(device,vkGetDeviceQueue);LOAD_DEVICE(device,vkCreateImage);LOAD_DEVICE(device,vkDestroyImage);LOAD_DEVICE(device,vkGetImageMemoryRequirements);LOAD_DEVICE(device,vkBindImageMemory);LOAD_DEVICE(device,vkCreateBuffer);LOAD_DEVICE(device,vkDestroyBuffer);LOAD_DEVICE(device,vkGetBufferMemoryRequirements);LOAD_DEVICE(device,vkAllocateMemory);LOAD_DEVICE(device,vkFreeMemory);LOAD_DEVICE(device,vkBindBufferMemory);LOAD_DEVICE(device,vkMapMemory);LOAD_DEVICE(device,vkUnmapMemory);LOAD_DEVICE(device,vkFlushMappedMemoryRanges);LOAD_DEVICE(device,vkInvalidateMappedMemoryRanges);LOAD_DEVICE(device,vkCreateCommandPool);LOAD_DEVICE(device,vkDestroyCommandPool);LOAD_DEVICE(device,vkResetCommandPool);LOAD_DEVICE(device,vkAllocateCommandBuffers);LOAD_DEVICE(device,vkBeginCommandBuffer);LOAD_DEVICE(device,vkEndCommandBuffer);LOAD_DEVICE(device,vkCmdPipelineBarrier);LOAD_DEVICE(device,vkCmdCopyBufferToImage);LOAD_DEVICE(device,vkCmdCopyImageToBuffer);LOAD_DEVICE(device,vkCmdBlitImage);LOAD_DEVICE(device,vkCmdExecuteCommands);LOAD_DEVICE(device,vkCreateFence);LOAD_DEVICE(device,vkDestroyFence);LOAD_DEVICE(device,vkQueueSubmit);LOAD_DEVICE(device,vkWaitForFences);LOAD_DEVICE(device,vkDeviceWaitIdle);
    NEED(vkDestroyDevice);NEED(vkGetDeviceQueue);NEED(vkCreateImage);NEED(vkDestroyImage);NEED(vkGetImageMemoryRequirements);NEED(vkBindImageMemory);NEED(vkCreateBuffer);NEED(vkDestroyBuffer);NEED(vkGetBufferMemoryRequirements);NEED(vkAllocateMemory);NEED(vkFreeMemory);NEED(vkBindBufferMemory);NEED(vkMapMemory);NEED(vkUnmapMemory);NEED(vkFlushMappedMemoryRanges);NEED(vkInvalidateMappedMemoryRanges);NEED(vkCreateCommandPool);NEED(vkDestroyCommandPool);NEED(vkResetCommandPool);NEED(vkAllocateCommandBuffers);NEED(vkBeginCommandBuffer);NEED(vkEndCommandBuffer);NEED(vkCmdPipelineBarrier);NEED(vkCmdCopyBufferToImage);NEED(vkCmdCopyImageToBuffer);NEED(vkCmdBlitImage);NEED(vkCmdExecuteCommands);NEED(vkCreateFence);NEED(vkDestroyFence);NEED(vkQueueSubmit);NEED(vkWaitForFences);NEED(vkDeviceWaitIdle);
    LOG("CALL: vkGetDeviceQueue(family=%u,index=0)\n",family);p_vkGetDeviceQueue(device,family,0,&queue);if(!queue){status=8;first_failure="vkGetDeviceQueue returned null";ERR("FAIL: vkGetDeviceQueue returned VK_NULL_HANDLE.\n");goto cleanup;}LOG("RESULT: vkGetDeviceQueue => non-null queue\n");
    if(!make_image(device,&mp,SRC_W,SRC_H,1,1,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,"sampled source (no storage)",&source)){status=9;first_failure="source image allocation/bind";goto cleanup;}
    if(!make_image(device,&mp,cfg.atlas_size,cfg.atlas_size,1,1,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,"ORIGINAL large atlas (no storage)",&atlas)){status=9;first_failure="atlas image allocation/bind";goto cleanup;}
    if(cfg.mip_layer&&!make_image(device,&mp,64,48,2,2,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,"separate mip/layer image (no storage)",&extra.image)){status=9;first_failure="mip-layer image allocation/bind";goto cleanup;}
    if(!make_host_buffer(device,&mp,total_bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,"full-atlas real upload staging",&upload)){status=9;first_failure="upload buffer allocation/bind";goto cleanup;}
    if(!make_host_buffer(device,&mp,total_bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,"full-atlas exact readback",&readback)){status=9;first_failure="readback buffer allocation/bind";goto cleanup;}
    if(!fill_upload(device,&upload,cfg.atlas_size,atlas_bytes,source_base,&extra,cfg.mip_layer)){status=9;first_failure="full atlas staging initialization";goto cleanup;}
    {VkCommandPoolCreateInfo pi;VkCommandBufferAllocateInfo ai;memset(&pi,0,sizeof(pi));pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pi.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;pi.queueFamilyIndex=family;LOG("CALL: vkCreateCommandPool(batch)\n");if(!checked(p_vkCreateCommandPool(device,&pi,NULL,&pool),"vkCreateCommandPool")){status=9;first_failure="command pool";goto cleanup;}memset(&ai,0,sizeof(ai));ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ai.commandPool=pool;ai.level=cfg.clone_secondary?VK_COMMAND_BUFFER_LEVEL_SECONDARY:VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;LOG("CALL: vkAllocateCommandBuffers(public worker)\n");if(!checked(p_vkAllocateCommandBuffers(device,&ai,&worker),"vkAllocateCommandBuffers(worker)")){status=9;first_failure="worker command buffer";goto cleanup;}if(cfg.clone_secondary){ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;LOG("CALL: vkAllocateCommandBuffers(primary wrapper)\n");if(!checked(p_vkAllocateCommandBuffers(device,&ai,&primary),"vkAllocateCommandBuffers(primary)")){status=9;first_failure="primary command buffer";goto cleanup;}}}
    LOG("BATCH FIRST: recording %u tiles BEFORE submit; %s; atlas allocation remains original no-STORAGE representation.\n",cfg.tiles,cfg.multi_region?"first 3 are one real multi-region call, remaining calls are individual":"all calls are individual");
    QueryPerformanceCounter(&timing_start);if(!record_first(worker,primary,cfg.clone_secondary,source.image,atlas.image,&upload,&readback,cfg.atlas_size,source_base,tiles,cfg.tiles,cfg.multi_region,&extra,cfg.mip_layer)){status=9;first_failure="record first batch";goto cleanup;}QueryPerformanceCounter(&timing_recorded);submit_cb=cfg.clone_secondary?primary:worker;if(!submit_wait(device,queue,submit_cb)){status=9;first_failure="first batch submit/wait";goto cleanup;}QueryPerformanceCounter(&timing_submitted);
    if(!verify_atlas(device,&readback,tiles,cfg.tiles,cfg.atlas_size,"FIRST")){status=9;first_failure="first full atlas pixel check";goto cleanup;}LOG("ATLAS_FIRST_FULL_PIXEL_CHECK: PASS\n");
    if(cfg.mip_layer){if(!verify_extra(device,&readback,&extra)){status=9;first_failure="mip/layer full pixel check";goto cleanup;}LOG("ATLAS_MIP_LAYER_FULL_PIXEL_CHECK: PASS\n");}
    QueryPerformanceCounter(&timing_verified);LOG("[ATLAS-BATCH-TIMING] batch=first tiles=%u recordMs=%.3f submitWaitMs=%.3f verifyMs=%.3f totalMs=%.3f diagnosticOnly=yes\n",cfg.tiles,qpc_ms(timing_start,timing_recorded,qpc_frequency),qpc_ms(timing_recorded,timing_submitted,qpc_frequency),qpc_ms(timing_submitted,timing_verified,qpc_frequency),qpc_ms(timing_start,timing_verified,qpc_frequency));
    LOG("CALL: vkResetCommandPool(after first fence, public command buffers retained)\n");if(!checked(p_vkResetCommandPool(device,pool,0),"vkResetCommandPool")){status=9;first_failure="vkResetCommandPool";goto cleanup;}LOG("ATLAS_POOL_RESET_REUSE: PASS\n");
    LOG("BATCH SECOND: reusing command pool and retained public command-buffer handle after first fence.\n");
    QueryPerformanceCounter(&timing_start);if(!record_second(worker,primary,cfg.clone_secondary,source.image,atlas.image,&readback,cfg.atlas_size,tiles,cfg.tiles)){status=9;first_failure="record second batch after pool reset";goto cleanup;}QueryPerformanceCounter(&timing_recorded);submit_cb=cfg.clone_secondary?primary:worker;if(!submit_wait(device,queue,submit_cb)){status=9;first_failure="second batch submit/wait";goto cleanup;}QueryPerformanceCounter(&timing_submitted);
    if(!verify_atlas(device,&readback,tiles,cfg.tiles,cfg.atlas_size,"SECOND")){status=9;first_failure="second full atlas pixel check";goto cleanup;}LOG("ATLAS_SECOND_FULL_PIXEL_CHECK: PASS\n");QueryPerformanceCounter(&timing_verified);LOG("[ATLAS-BATCH-TIMING] batch=second tiles=%u recordMs=%.3f submitWaitMs=%.3f verifyMs=%.3f totalMs=%.3f diagnosticOnly=yes\n",cfg.tiles,qpc_ms(timing_start,timing_recorded,qpc_frequency),qpc_ms(timing_recorded,timing_submitted,qpc_frequency),qpc_ms(timing_submitted,timing_verified,qpc_frequency),qpc_ms(timing_start,timing_verified,qpc_frequency));
    status=0;
cleanup:
    if(device&&p_vkDeviceWaitIdle){LOG("CALL: vkDeviceWaitIdle(cleanup)\n");if(!checked(p_vkDeviceWaitIdle(device),"vkDeviceWaitIdle(cleanup)")&&status==0){status=10;first_failure="vkDeviceWaitIdle cleanup";}}
    if(pool&&p_vkDestroyCommandPool)p_vkDestroyCommandPool(device,pool,NULL);
    if(device&&p_vkDestroyImage){destroy_buffer(device,&readback);destroy_buffer(device,&upload);destroy_image(device,&extra.image);destroy_image(device,&atlas);destroy_image(device,&source);}
    if(device&&p_vkDestroyDevice)p_vkDestroyDevice(device,NULL);
    free(adapters);
    if(instance&&p_vkDestroyInstance)p_vkDestroyInstance(instance,NULL);
    if(loader)FreeLibrary(loader);
    if(status)ERR("FINAL: FAIL status=%d; FIRST_FAILURE: %s\n",status,first_failure?first_failure:"unspecified");else LOG("FINAL: ATLAS BATCH PASS\n");
    return status;
}
