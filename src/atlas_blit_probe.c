/*
 * Windows x64 raw-Vulkan vkCmdBlitImage diagnostic.
 * No Vulkan import library is used: Vulkan entry points come only from the
 * caller-supplied loader through vkGetInstanceProcAddr/vkGetDeviceProcAddr.
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
#define DST_W 32u
#define DST_H 32u
#define SRC_UPLOAD_OFFSET 0u
#define DST_UPLOAD_OFFSET 512u
#define STAGING_SIZE (DST_UPLOAD_OFFSET + DST_W * DST_H * 4u)
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define ERR(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

typedef struct ImageResource { VkImage image; VkDeviceMemory memory; } ImageResource;
typedef struct BufferResource { VkBuffer buffer; VkDeviceMemory memory; VkDeviceSize size; } BufferResource;
typedef struct ProbeConfig { int source_needs_temp; int destination_direct; int iterations; int clone_secondary; int bc7_source; } ProbeConfig;
typedef struct BlitCase {
    const char *name;
    int sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1;
    uint8_t rgba[4];
} BlitCase;

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkEnumerateInstanceVersion p_vkEnumerateInstanceVersion;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkDestroyInstance p_vkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceFeatures p_vkGetPhysicalDeviceFeatures;
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
static PFN_vkCmdExecuteCommands p_vkCmdExecuteCommands;
static PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
static PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
static PFN_vkCmdCopyBufferToImage p_vkCmdCopyBufferToImage;
static PFN_vkCmdCopyImage p_vkCmdCopyImage;
static PFN_vkCmdCopyImageToBuffer p_vkCmdCopyImageToBuffer;
static PFN_vkCmdBlitImage p_vkCmdBlitImage;
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

static int checked(VkResult r, const char *label) {
    LOG("RESULT: %s => %s (%d)\n", label, result_name(r), (int)r);
    return r == VK_SUCCESS;
}

static int absolute_windows_path(const wchar_t *p) {
    return p && ((p[0] == L'\\' && p[1] == L'\\') ||
        (iswalpha((wint_t)p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')));
}

static int ci_contains(const char *s, const char *part) {
    size_t n = strlen(part), i;
    for (; *s; ++s) {
        for (i = 0; i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)part[i]); ++i) {}
        if (i == n) return 1;
    }
    return !n;
}

static int software_like(const VkPhysicalDeviceProperties *p) {
    return p->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU || ci_contains(p->deviceName, "warp") ||
        ci_contains(p->deviceName, "swiftshader") || ci_contains(p->deviceName, "lavapipe") ||
        ci_contains(p->deviceName, "llvmpipe") || ci_contains(p->deviceName, "software") ||
        ci_contains(p->deviceName, "basic render");
}

static const char *device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "DISCRETE_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "VIRTUAL_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
    default: return "OTHER";
    }
}

static int instance_extension_present(const char *wanted) {
    uint32_t count = 0, i; VkExtensionProperties *list = NULL; int found = 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) return 0;
    list = count ? (VkExtensionProperties *)calloc(count, sizeof(*list)) : NULL;
    if (count && !list) return 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, list) == VK_SUCCESS)
        for (i = 0; i < count; ++i) if (!strcmp(list[i].extensionName, wanted)) { found = 1; break; }
    free(list); return found;
}

static int device_extension_present(VkPhysicalDevice pd, const char *wanted) {
    uint32_t count = 0, i; VkExtensionProperties *list = NULL; int found = 0;
    if (p_vkEnumerateDeviceExtensionProperties(pd, NULL, &count, NULL) != VK_SUCCESS) return 0;
    list = count ? (VkExtensionProperties *)calloc(count, sizeof(*list)) : NULL;
    if (count && !list) return 0;
    if (p_vkEnumerateDeviceExtensionProperties(pd, NULL, &count, list) == VK_SUCCESS)
        for (i = 0; i < count; ++i) if (!strcmp(list[i].extensionName, wanted)) { found = 1; break; }
    free(list); return found;
}

static int find_transfer_queue(VkPhysicalDevice pd, uint32_t *out) {
    uint32_t n = 0, i; VkQueueFamilyProperties *q;
    p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    q = n ? (VkQueueFamilyProperties *)calloc(n, sizeof(*q)) : NULL;
    if (!q) return 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, q);
    for (i = 0; i < n; ++i) {
        LOG("QUEUE[%u]: flags=0x%X count=%u\n", i, q[i].queueFlags, q[i].queueCount);
        if (q[i].queueCount && (q[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) { *out = i; free(q); return 1; }
    }
    free(q); return 0;
}

static int memory_type(uint32_t bits, const VkPhysicalDeviceMemoryProperties *props, VkMemoryPropertyFlags required) {
    uint32_t i;
    for (i = 0; i < props->memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props->memoryTypes[i].propertyFlags & required) == required) return (int)i;
    return -1;
}

static void destroy_image(VkDevice device, ImageResource *r) {
    if (r->image) p_vkDestroyImage(device, r->image, NULL);
    if (r->memory) p_vkFreeMemory(device, r->memory, NULL);
    r->image = VK_NULL_HANDLE; r->memory = VK_NULL_HANDLE;
}
static void destroy_buffer(VkDevice device, BufferResource *r) {
    if (r->buffer) p_vkDestroyBuffer(device, r->buffer, NULL);
    if (r->memory) p_vkFreeMemory(device, r->memory, NULL);
    r->buffer = VK_NULL_HANDLE; r->memory = VK_NULL_HANDLE;
}

static int make_image(VkDevice device, const VkPhysicalDeviceMemoryProperties *mp, uint32_t width, uint32_t height,
                      VkImageUsageFlags usage, VkFormat format, const char *name, ImageResource *out) {
    VkImageCreateInfo ci; VkMemoryRequirements req; VkMemoryAllocateInfo ai; int mt;
    memset(out, 0, sizeof(*out)); memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format;
    ci.extent.width = width; ci.extent.height = height; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    LOG("CALL: vkCreateImage(%s RGBA8 %ux%u usage=0x%X)\n", name, width, height, usage);
    if (!checked(p_vkCreateImage(device, &ci, NULL, &out->image), "vkCreateImage")) return 0;
    p_vkGetImageMemoryRequirements(device, out->image, &req);
    LOG("REQUIREMENTS %s: size=%llu alignment=%llu typeBits=0x%08X\n", name, (unsigned long long)req.size,
        (unsigned long long)req.alignment, req.memoryTypeBits);
    mt = memory_type(req.memoryTypeBits, mp, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { ERR("FAIL: %s has no DEVICE_LOCAL memory type; no fallback is substituted.\n", name); return 0; }
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)mt;
    LOG("CALL: vkAllocateMemory(%s DEVICE_LOCAL type=%d flags=0x%X)\n", name, mt, mp->memoryTypes[mt].propertyFlags);
    if (!checked(p_vkAllocateMemory(device, &ai, NULL, &out->memory), "vkAllocateMemory(image)")) return 0;
    LOG("CALL: vkBindImageMemory(%s offset=0)\n", name);
    if (!checked(p_vkBindImageMemory(device, out->image, out->memory, 0), "vkBindImageMemory")) return 0;
    return 1;
}

static int make_host_buffer(VkDevice device, const VkPhysicalDeviceMemoryProperties *mp, VkDeviceSize size,
                            VkBufferUsageFlags usage, const char *name, BufferResource *out) {
    VkBufferCreateInfo ci; VkMemoryRequirements req; VkMemoryAllocateInfo ai; int mt;
    memset(out, 0, sizeof(*out)); memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    LOG("CALL: vkCreateBuffer(%s size=%llu usage=0x%X)\n", name, (unsigned long long)size, usage);
    if (!checked(p_vkCreateBuffer(device, &ci, NULL, &out->buffer), "vkCreateBuffer")) return 0;
    p_vkGetBufferMemoryRequirements(device, out->buffer, &req);
    mt = memory_type(req.memoryTypeBits, mp, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    LOG("REQUIREMENTS %s: size=%llu alignment=%llu typeBits=0x%08X\n", name, (unsigned long long)req.size,
        (unsigned long long)req.alignment, req.memoryTypeBits);
    if (mt < 0) { ERR("FAIL: %s has no HOST_VISIBLE|HOST_COHERENT memory type.\n", name); return 0; }
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)mt;
    LOG("CALL: vkAllocateMemory(%s host-coherent type=%d flags=0x%X)\n", name, mt, mp->memoryTypes[mt].propertyFlags);
    if (!checked(p_vkAllocateMemory(device, &ai, NULL, &out->memory), "vkAllocateMemory(buffer)")) return 0;
    LOG("CALL: vkBindBufferMemory(%s offset=0)\n", name);
    if (!checked(p_vkBindBufferMemory(device, out->buffer, out->memory, 0), "vkBindBufferMemory")) return 0;
    out->size = req.size;
    return 1;
}

static VkImageSubresourceLayers color_layers(void) {
    VkImageSubresourceLayers s; memset(&s, 0, sizeof(s)); s.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; s.layerCount = 1; return s;
}

static void transition(VkCommandBuffer cb, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                       VkAccessFlags src_access, VkAccessFlags dst_access, const char *label) {
    VkImageMemoryBarrier b; VkPipelineStageFlags src_stage = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    memset(&b, 0, sizeof(b)); b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.oldLayout = old_layout; b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
    LOG("CALL: vkCmdPipelineBarrier(%s)\n", label);
    p_vkCmdPipelineBarrier(cb, src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    LOG("RESULT: vkCmdPipelineBarrier(%s) => recorded\n", label);
}

static void copy_buffer_to_image(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkImage image, uint32_t width, uint32_t height, const char *label) {
    VkBufferImageCopy r; memset(&r, 0, sizeof(r)); r.bufferOffset = offset; r.imageSubresource = color_layers();
    r.imageExtent.width = width; r.imageExtent.height = height; r.imageExtent.depth = 1;
    LOG("CALL: vkCmdCopyBufferToImage(%s offset=%llu extent=%ux%u)\n", label, (unsigned long long)offset, width, height);
    p_vkCmdCopyBufferToImage(cb, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    LOG("RESULT: vkCmdCopyBufferToImage(%s) => recorded\n", label);
}


static int write_staging(VkDevice device, BufferResource *staging, const BlitCase *bc, int bc7_source) {
    static const uint8_t src_background[4] = { 0x21, 0x35, 0x49, 0x5D };
    static const uint8_t dst_background[4] = { 0xA1, 0xB2, 0xC3, 0xD4 };
    uint8_t *p; uint32_t x, y; VkMappedMemoryRange range;
    LOG("CALL: vkMapMemory(host staging)\n");
    if (!checked(p_vkMapMemory(device, staging->memory, 0, staging->size, 0, (void **)&p), "vkMapMemory(staging)")) return 0;
    memset(p, 0xCD, (size_t)staging->size);
    for (y = 0; y < SRC_H; ++y) for (x = 0; x < SRC_W; ++x) {
        uint8_t *q = p + SRC_UPLOAD_OFFSET + (y * SRC_W + x) * 4u;
        memcpy(q, src_background, 4);
        if ((int)x >= bc->sx0 && (int)x < bc->sx1 && (int)y >= bc->sy0 && (int)y < bc->sy1) memcpy(q, bc->rgba, 4);
    }
    for (y = 0; y < DST_H; ++y) for (x = 0; x < DST_W; ++x) memcpy(p + DST_UPLOAD_OFFSET + (y * DST_W + x) * 4u, dst_background, 4);
    if (bc7_source) {
        /* Valid mode 6: two identical 7-bit RGBA endpoints, zero p-bits,
           and all interpolation indices zero. Every decoded channel is exact/even. */
        uint8_t block[16] = {0}; unsigned bit = 0;
        unsigned field_values[12] = {64, bc->rgba[0]/2u, bc->rgba[0]/2u, bc->rgba[1]/2u, bc->rgba[1]/2u, bc->rgba[2]/2u, bc->rgba[2]/2u, bc->rgba[3]/2u, bc->rgba[3]/2u, 0, 0, 0};
        unsigned field_widths[12] = {7,7,7,7,7,7,7,7,7,1,1,3};
        for (unsigned f=0;f<12;++f) for (unsigned k=0;k<field_widths[f];++k,++bit)
            if (field_values[f] & (1u<<k)) block[bit/8u] |= (uint8_t)(1u<<(bit%8u));
        for (unsigned b=0;b<((SRC_W+3u)/4u)*((SRC_H+3u)/4u);++b) memcpy(p+b*16u,block,16u);
        LOG("BC7 mode-6 source: valid identical endpoints, exact RGBA=%u,%u,%u,%u; uploaded compressed blocks, not CPU-decoded pixels.\n", bc->rgba[0],bc->rgba[1],bc->rgba[2],bc->rgba[3]);
    }
    memset(&range, 0, sizeof(range)); range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = staging->memory; range.size = VK_WHOLE_SIZE;
    LOG("CALL: vkFlushMappedMemoryRanges(host staging)\n");
    if (!checked(p_vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges(staging)")) { p_vkUnmapMemory(device, staging->memory); return 0; }
    p_vkUnmapMemory(device, staging->memory);
    LOG("RESULT: vkUnmapMemory(staging) => void; source is solid RGBA rect over known background, destination is known RGBA sentinel.\n");
    return 1;
}

static int verify_readback(VkDevice device, BufferResource *readback, const BlitCase *bc) {
    static const uint8_t dst_background[4] = { 0xA1, 0xB2, 0xC3, 0xD4 };
    uint8_t *p; VkMappedMemoryRange range; uint32_t x, y;
    LOG("CALL: vkMapMemory(readback full 32x32 RGBA8)\n");
    if (!checked(p_vkMapMemory(device, readback->memory, 0, readback->size, 0, (void **)&p), "vkMapMemory(readback)")) return 0;
    memset(&range, 0, sizeof(range)); range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = readback->memory; range.size = VK_WHOLE_SIZE;
    LOG("CALL: vkInvalidateMappedMemoryRanges(readback after fence)\n");
    if (!checked(p_vkInvalidateMappedMemoryRanges(device, 1, &range), "vkInvalidateMappedMemoryRanges(readback)")) { p_vkUnmapMemory(device, readback->memory); return 0; }
    for (y = 0; y < DST_H; ++y) for (x = 0; x < DST_W; ++x) {
        const uint8_t *expected = ((int)x >= bc->dx0 && (int)x < bc->dx1 && (int)y >= bc->dy0 && (int)y < bc->dy1) ? bc->rgba : dst_background;
        const uint8_t *actual = p + (y * DST_W + x) * 4u;
        if (memcmp(actual, expected, 4)) {
            ERR("FAIL: exact readback mismatch at (%u,%u): got %02X %02X %02X %02X expected %02X %02X %02X %02X.\n", x, y,
                actual[0], actual[1], actual[2], actual[3], expected[0], expected[1], expected[2], expected[3]);
            p_vkUnmapMemory(device, readback->memory); return 0;
        }
    }
    p_vkUnmapMemory(device, readback->memory);
    LOG("RESULT: vkUnmapMemory(readback) => void\n");
    LOG("PIXEL PASS: exact target rectangle and every unchanged outside sentinel pixel matched (no tolerance, no synthesized pixels).\n");
    return 1;
}

static int run_case(VkDevice device, VkQueue queue, uint32_t family, const VkPhysicalDeviceMemoryProperties *mp,
                    const ProbeConfig *cfg, unsigned iteration, const BlitCase *bc) {
    ImageResource source = {0}, destination = {0}, temp_source = {0}, temp_destination = {0};
    BufferResource staging = {0}, readback = {0}; VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci; VkCommandBufferAllocateInfo cai; VkCommandBufferBeginInfo cbi; VkFenceCreateInfo fci; VkSubmitInfo si;
    VkImage actual_source, actual_destination; VkImageBlit blit; VkBufferImageCopy rb; VkBufferMemoryBarrier host_barrier;
    VkImageUsageFlags source_usage, destination_usage; int ok = 0;
    VkCommandBuffer primary = VK_NULL_HANDLE;
    VkCommandBufferInheritanceInfo inheritance = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_FALSE, 0, 0};
    BlitCase payload = *bc;
    if (cfg->bc7_source) { payload.rgba[0]=16;payload.rgba[1]=226;payload.rgba[2]=52;payload.rgba[3]=254;bc=&payload; }
    const int need_source_temp = cfg->source_needs_temp;
    const int need_destination_temp = !cfg->destination_direct;
    LOG("\nCASE %u/%d %s: sourceRect=(%d,%d)-(%d,%d), destinationRect=(%d,%d)-(%d,%d), filter=NEAREST\n",
        iteration, cfg->iterations, bc->name, bc->sx0, bc->sy0, bc->sx1, bc->sy1, bc->dx0, bc->dy0, bc->dx1, bc->dy1);
    LOG("KNOWN-FIRST MODE: source%s, destination%s; temporary source=%s temporary destination=%s.\n",
        need_source_temp ? " TRANSFER_SRC|TRANSFER_DST (no SAMPLED)" : " TRANSFER_SRC|TRANSFER_DST|SAMPLED",
        cfg->destination_direct ? " TRANSFER_DST|TRANSFER_SRC|COLOR_ATTACHMENT|STORAGE" : " TRANSFER_DST|TRANSFER_SRC|COLOR_ATTACHMENT (no STORAGE)",
        need_source_temp ? "YES (full-image copy)" : "NO", need_destination_temp ? "YES" : "NO");
    source_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (need_source_temp ? 0 : VK_IMAGE_USAGE_SAMPLED_BIT);
    destination_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        (cfg->destination_direct ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    if (!make_image(device, mp, SRC_W, SRC_H, source_usage, cfg->bc7_source ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM, "logical source", &source)) goto cleanup;
    if (!make_image(device, mp, DST_W, DST_H, destination_usage, VK_FORMAT_R8G8B8A8_UNORM, "logical destination", &destination)) goto cleanup;
    if (!make_host_buffer(device, mp, STAGING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "host upload staging", &staging)) goto cleanup;
    if (!make_host_buffer(device, mp, DST_W * DST_H * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "host readback", &readback)) goto cleanup;
    if (!write_staging(device, &staging, bc, cfg->bc7_source)) goto cleanup;

    memset(&pci, 0, sizeof(pci)); pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pci.queueFamilyIndex = family;
    LOG("CALL: vkCreateCommandPool(case transfer)\n");
    if (!checked(p_vkCreateCommandPool(device, &pci, NULL, &pool), "vkCreateCommandPool")) goto cleanup;
    memset(&cai, 0, sizeof(cai)); cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cai.commandPool = pool; cai.level = cfg->clone_secondary ? VK_COMMAND_BUFFER_LEVEL_SECONDARY : VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    LOG("CALL: vkAllocateCommandBuffers(case)\n");
    if (!checked(p_vkAllocateCommandBuffers(device, &cai, &cb), "vkAllocateCommandBuffers")) goto cleanup;
    memset(&cbi, 0, sizeof(cbi)); cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (cfg->clone_secondary) cbi.pInheritanceInfo = &inheritance;
    LOG("CALL: vkBeginCommandBuffer(case)\n");
    if (!checked(p_vkBeginCommandBuffer(cb, &cbi), "vkBeginCommandBuffer")) goto cleanup;

    transition(cb, source.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, "logical source UNDEFINED->TRANSFER_DST");
    transition(cb, destination.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, "logical destination UNDEFINED->TRANSFER_DST");
    copy_buffer_to_image(cb, staging.buffer, SRC_UPLOAD_OFFSET, source.image, SRC_W, SRC_H, "host staging -> logical source");
    copy_buffer_to_image(cb, staging.buffer, DST_UPLOAD_OFFSET, destination.image, DST_W, DST_H, "host staging -> logical destination sentinel");
    transition(cb, source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, "logical source TRANSFER_DST->TRANSFER_SRC");
    actual_source = source.image;
    actual_destination = destination.image;
    memset(&blit, 0, sizeof(blit)); blit.srcSubresource = color_layers(); blit.dstSubresource = color_layers();
    blit.srcOffsets[0].x = bc->sx0; blit.srcOffsets[0].y = bc->sy0; blit.srcOffsets[0].z = 0;
    blit.srcOffsets[1].x = bc->sx1; blit.srcOffsets[1].y = bc->sy1; blit.srcOffsets[1].z = 1;
    blit.dstOffsets[0].x = bc->dx0; blit.dstOffsets[0].y = bc->dy0; blit.dstOffsets[0].z = 0;
    blit.dstOffsets[1].x = bc->dx1; blit.dstOffsets[1].y = bc->dy1; blit.dstOffsets[1].z = 1;
    LOG("CALL: vkCmdBlitImage(ACTUAL RAW VULKAN %s; source=%s destination=%s; NEAREST)\n", bc->name, need_source_temp ? "logical source requiring driver temp" : "logical sampled source", need_destination_temp ? "logical destination requiring driver temp" : "logical storage destination");
    p_vkCmdBlitImage(cb, actual_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, actual_destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    LOG("RESULT: vkCmdBlitImage => recorded (vkEndCommandBuffer/submit failures propagate below)\n");
    transition(cb, actual_destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, "actual blit destination TRANSFER_DST->TRANSFER_SRC");
    memset(&rb, 0, sizeof(rb)); rb.imageSubresource = color_layers(); rb.imageExtent.width = DST_W; rb.imageExtent.height = DST_H; rb.imageExtent.depth = 1;
    LOG("CALL: vkCmdCopyImageToBuffer(logical destination full 32x32 -> host readback)\n");
    p_vkCmdCopyImageToBuffer(cb, destination.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &rb);
    LOG("RESULT: vkCmdCopyImageToBuffer => recorded\n");
    memset(&host_barrier, 0, sizeof(host_barrier)); host_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT; host_barrier.buffer = readback.buffer; host_barrier.size = VK_WHOLE_SIZE;
    LOG("CALL: vkCmdPipelineBarrier(readback TRANSFER_WRITE->HOST_READ)\n");
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host_barrier, 0, NULL);
    LOG("RESULT: vkCmdPipelineBarrier(readback->host) => recorded\n");
    LOG("CALL: vkEndCommandBuffer(after actual vkCmdBlitImage)\n");
    if (!checked(p_vkEndCommandBuffer(cb), "vkEndCommandBuffer")) goto cleanup;
    if (cfg->clone_secondary) {
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        LOG("CALL: vkAllocateCommandBuffers(primary clone target)\n");
        if (!checked(p_vkAllocateCommandBuffers(device, &cai, &primary), "vkAllocateCommandBuffers(primary)")) goto cleanup;
        cbi.pInheritanceInfo = NULL;
        if (!checked(p_vkBeginCommandBuffer(primary, &cbi), "vkBeginCommandBuffer(primary)")) goto cleanup;
        LOG("CALL: vkCmdExecuteCommands(real recorded secondary atlas batch)\n");
        p_vkCmdExecuteCommands(primary, 1, &cb);
        if (!checked(p_vkEndCommandBuffer(primary), "vkEndCommandBuffer(primary clone)")) goto cleanup;
    }
    memset(&fci, 0, sizeof(fci)); fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    LOG("CALL: vkCreateFence(case)\n");
    if (!checked(p_vkCreateFence(device, &fci, NULL, &fence), "vkCreateFence")) goto cleanup;
    memset(&si, 0, sizeof(si)); si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = cfg->clone_secondary ? &primary : &cb;
    LOG("CALL: vkQueueSubmit(after actual vkCmdBlitImage)\n");
    if (!checked(p_vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit")) goto cleanup;
    LOG("CALL: vkWaitForFences(case)\n");
    if (!checked(p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences")) goto cleanup;
    if (!verify_readback(device, &readback, bc)) goto cleanup;
    LOG("CALL: vkResetCommandPool(after fence and exact pixels)\n");
    if (!checked(p_vkResetCommandPool(device, pool, 0), "vkResetCommandPool")) goto cleanup;
    memset(&cbi, 0, sizeof(cbi)); cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (cfg->clone_secondary) cbi.pInheritanceInfo = &inheritance;
    if (!checked(p_vkBeginCommandBuffer(cb, &cbi), "vkBeginCommandBuffer(retained handle after pool reset)")) goto cleanup;
    if (!checked(p_vkEndCommandBuffer(cb), "vkEndCommandBuffer(retained handle after reset)")) goto cleanup;
    LOG("LIFETIME PASS: pool reset released atlas temporaries while retaining valid allocated command-buffer handle.\n");
    ok = 1;
cleanup:
    /* Submitted command resources remain alive through fence; wait idle before every image/memory destruction. */
    if (device && p_vkDeviceWaitIdle) {
        LOG("CALL: vkDeviceWaitIdle(case cleanup before destroy)\n");
        if (!checked(p_vkDeviceWaitIdle(device), "vkDeviceWaitIdle(case cleanup)")) ok = 0;
    }
    if (fence) p_vkDestroyFence(device, fence, NULL);
    if (pool) p_vkDestroyCommandPool(device, pool, NULL);
    destroy_buffer(device, &readback); destroy_buffer(device, &staging);
    destroy_image(device, &temp_destination); destroy_image(device, &temp_source);
    destroy_image(device, &destination); destroy_image(device, &source);
    if (!ok) ERR("CASE FAIL: %s iteration %u; Vulkan errors from command end/submit/wait are never treated as a pass.\n", bc->name, iteration);
    else LOG("CASE PASS: %s iteration %u; submit/wait/destroy lifecycle completed.\n", bc->name, iteration);
    return ok;
}

static void usage(const wchar_t *exe) {
    LOG("Usage:\n  %ls --loader C:\\absolute\\vulkan-1.dll --icd C:\\absolute\\ashesD3D11Renderer_icd.json [options]\n", exe);
    LOG("Options: --iterations N (1..8; default 3), --source-needs-temp, --destination-direct, --clone-secondary, --bc7-source, --allow-non-intel-test\n");
    LOG("Default requires a non-software Intel adapter. --allow-non-intel-test is sandbox-only and is not Intel hardware validation.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL; int allow_non_intel = 0, i, status = 1; const char *first_failure = NULL;
    ProbeConfig cfg; HMODULE loader = NULL; union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } proc;
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice *devices = NULL, physical = VK_NULL_HANDLE; uint32_t device_count = 0, family = UINT32_MAX;
    VkPhysicalDeviceProperties selected; VkPhysicalDeviceMemoryProperties memory_props; VkDevice device = VK_NULL_HANDLE; VkQueue queue = VK_NULL_HANDLE;
    static const BlitCase cases[] = {
        { "scaled odd 7x5-to-13x9 nonzero destination origin", 1, 1, 8, 6, 3, 5, 16, 14, { 0x10, 0xE1, 0x34, 0xFF } },
        { "nonzero source origin scaled 5x4-to-11x11", 2, 1, 7, 5, 17, 2, 28, 13, { 0xB4, 0x26, 0xD0, 0x7C } },
        { "nonzero source origin scaled 7x3-to-13x7", 1, 3, 8, 6, 7, 20, 20, 27, { 0x5A, 0xC8, 0x19, 0xE3 } }
    };
    memset(&cfg, 0, sizeof(cfg)); cfg.iterations = 3; memset(&selected, 0, sizeof(selected)); memset(&memory_props, 0, sizeof(memory_props));
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--bc7-source")) cfg.bc7_source = 1;
        else if (!wcscmp(argv[i], L"--clone-secondary")) cfg.clone_secondary = 1;
        else if (!wcscmp(argv[i], L"--source-needs-temp")) cfg.source_needs_temp = 1;
        else if (!wcscmp(argv[i], L"--destination-direct")) cfg.destination_direct = 1;
        else if (!wcscmp(argv[i], L"--iterations") && i + 1 < argc) {
            wchar_t *end = NULL; long n = wcstol(argv[++i], &end, 10);
            if (!end || *end || n < 1 || n > 8) { ERR("FAIL: --iterations must be an integer in 1..8.\n"); return 2; }
            cfg.iterations = (int)n;
        } else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { usage(argv[0]); return 0; }
        else { ERR("FAIL: unrecognized or incomplete argument: %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!loader_path || !icd_path || !absolute_windows_path(loader_path) || !absolute_windows_path(icd_path)) { ERR("FAIL: --loader and --icd must be mandatory absolute Windows paths.\n"); usage(argv[0]); return 2; }
    if (GetFileAttributesW(loader_path) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(icd_path) == INVALID_FILE_ATTRIBUTES) { ERR("FAIL: supplied loader or ICD path does not exist (GetLastError=%lu).\n", GetLastError()); return 2; }
    if (GetModuleHandleW(L"vulkan-1.dll")) { ERR("FAIL: vulkan-1.dll was already loaded; refusing ambiguous supplied-loader isolation.\n"); return 3; }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path) || !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) { ERR("FAIL: cannot set per-process ICD environment (GetLastError=%lu).\n", GetLastError()); return 3; }
    LOG("Raw Vulkan vkCmdBlitImage probe: supplied loader/ICD only; no Vulkan import library.\nLOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    LOG("CONFIG: iterations=%d source-needs-temp=%s destination-direct=%s\n", cfg.iterations, cfg.source_needs_temp ? "yes" : "no", cfg.destination_direct ? "yes" : "no");
    if (allow_non_intel) LOG("WARNING: --allow-non-intel-test accepts Wine/synthetic/non-Intel only for sandbox integration; NOT Intel validation.\n");
    loader = LoadLibraryW(loader_path);
    if (!loader) { ERR("FAIL: LoadLibraryW(loader) error %lu\n", GetLastError()); status = 4; first_failure = "LoadLibraryW(loader)"; goto cleanup; }
    proc.raw = GetProcAddress(loader, "vkGetInstanceProcAddr"); gipa = proc.typed;
    if (!gipa) { ERR("FAIL: supplied loader exposes no vkGetInstanceProcAddr.\n"); status = 4; first_failure = "vkGetInstanceProcAddr"; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion); LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties); LOAD_GLOBAL(vkCreateInstance);
    NEED(vkEnumerateInstanceExtensionProperties); NEED(vkCreateInstance);
    if (p_vkEnumerateInstanceVersion) { uint32_t v = VK_API_VERSION_1_0; LOG("CALL: vkEnumerateInstanceVersion\n"); if (!checked(p_vkEnumerateInstanceVersion(&v), "vkEnumerateInstanceVersion")) { status = 21; first_failure = "vkEnumerateInstanceVersion"; goto cleanup; } LOG("LOADER MAX API: %u.%u.%u; application requests 1.0.0\n", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v)); }
    {
        VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkCmdBlitImage raw probe", 1, "none", 1, VK_API_VERSION_1_0 };
        VkInstanceCreateInfo ci; const char *exts[1]; uint32_t extc = 0;
        memset(&ci, 0, sizeof(ci)); ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo = &ai;
        if (instance_extension_present(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) { ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; exts[extc++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME; LOG("PORTABILITY: enabling advertised VK_KHR_portability_enumeration.\n"); }
        ci.enabledExtensionCount = extc; ci.ppEnabledExtensionNames = exts;
        LOG("CALL: vkCreateInstance(api=1.0)\n"); if (!checked(p_vkCreateInstance(&ci, NULL, &instance), "vkCreateInstance")) { status = 21; first_failure = "vkCreateInstance"; goto cleanup; }
    }
    LOAD_INSTANCE(instance, vkDestroyInstance); LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices); LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceFeatures); LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceFormatProperties); LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties); LOAD_INSTANCE(instance, vkCreateDevice); LOAD_INSTANCE(instance, vkGetDeviceProcAddr);
    NEED(vkDestroyInstance); NEED(vkEnumeratePhysicalDevices); NEED(vkGetPhysicalDeviceProperties); NEED(vkGetPhysicalDeviceFeatures); NEED(vkGetPhysicalDeviceMemoryProperties); NEED(vkGetPhysicalDeviceQueueFamilyProperties); NEED(vkGetPhysicalDeviceFormatProperties); NEED(vkEnumerateDeviceExtensionProperties); NEED(vkCreateDevice); NEED(vkGetDeviceProcAddr);
    LOG("CALL: vkEnumeratePhysicalDevices(count)\n"); if (!checked(p_vkEnumeratePhysicalDevices(instance, &device_count, NULL), "vkEnumeratePhysicalDevices(count)")) { status = 21; first_failure = "vkEnumeratePhysicalDevices"; goto cleanup; }
    if (!device_count) { ERR("FAIL: ICD reports no physical devices.\n"); status = 5; first_failure = "zero adapters"; goto cleanup; }
    devices = (VkPhysicalDevice *)calloc(device_count, sizeof(*devices)); if (!devices) { status = 5; first_failure = "adapter list allocation"; goto cleanup; }
    LOG("CALL: vkEnumeratePhysicalDevices(list)\n"); if (!checked(p_vkEnumeratePhysicalDevices(instance, &device_count, devices), "vkEnumeratePhysicalDevices(list)")) { status = 21; first_failure = "vkEnumeratePhysicalDevices list"; goto cleanup; }
    for (i = 0; i < (int)device_count; ++i) { VkPhysicalDeviceProperties p; uint32_t f;
        p_vkGetPhysicalDeviceProperties(devices[i], &p); LOG("ADAPTER[%d]: %s vendor=0x%04X type=%s%s\n", i, p.deviceName, p.vendorID, device_type_name(p.deviceType), software_like(&p) ? " [software-like]" : "");
        if (!allow_non_intel && (p.vendorID != INTEL_VENDOR_ID || software_like(&p))) continue;
        if (!physical && find_transfer_queue(devices[i], &f)) { physical = devices[i]; family = f; selected = p; }
    }
    if (!physical) { ERR("FAIL: no eligible adapter. Default requires non-software Intel; --allow-non-intel-test is sandbox-only.\n"); status = 6; first_failure = "eligible Intel adapter selection"; goto cleanup; }
    if (selected.vendorID != INTEL_VENDOR_ID || software_like(&selected)) LOG("WARNING: selected non-Intel/software-like adapter; result is sandbox-only, NOT Intel validation.\n");
    LOG("SELECTED: %s vendor=0x%04X queueFamily=%u\n", selected.deviceName, selected.vendorID, family); p_vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    {
        VkFormatProperties fp; VkFormatFeatureFlags need = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        p_vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &fp); LOG("KNOWN-FIRST FORMAT RGBA8_UNORM optimalFeatures=0x%X need=0x%X (temp branches included)\n", fp.optimalTilingFeatures, need);
        if ((fp.optimalTilingFeatures & need) != need) { ERR("FAIL: RGBA8 optimal tiling lacks a required known feature; refusing a different representation.\n"); status = 7; first_failure = "known RGBA8 feature gate"; goto cleanup; }
    }
    {
        float priority = 1.0f; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &priority }; VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL }; const char *subset = "VK_KHR_portability_subset";
        VkPhysicalDeviceFeatures available_features = {0}, enabled_features = {0};
        p_vkGetPhysicalDeviceFeatures(physical, &available_features);
        if (cfg.bc7_source) {
            if (!available_features.textureCompressionBC) { status=7; first_failure="BC7 feature unavailable"; goto cleanup; }
            enabled_features.textureCompressionBC = VK_TRUE;
            dci.pEnabledFeatures = &enabled_features;
            VkFormatProperties source_properties;
            p_vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_BC7_UNORM_BLOCK, &source_properties);
            if (!(source_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) { status=7; first_failure="BC7 blit source capability unavailable"; goto cleanup; }
        }
        if (device_extension_present(physical, subset)) { dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = &subset; LOG("PORTABILITY: enabling advertised VK_KHR_portability_subset.\n"); }
        LOG("CALL: vkCreateDevice\n"); if (!checked(p_vkCreateDevice(physical, &dci, NULL, &device), "vkCreateDevice")) { status = 21; first_failure = "vkCreateDevice"; goto cleanup; }
    }
    LOAD_DEVICE(device, vkDestroyDevice); LOAD_DEVICE(device, vkGetDeviceQueue); LOAD_DEVICE(device, vkCreateImage); LOAD_DEVICE(device, vkDestroyImage); LOAD_DEVICE(device, vkGetImageMemoryRequirements); LOAD_DEVICE(device, vkBindImageMemory); LOAD_DEVICE(device, vkCreateBuffer); LOAD_DEVICE(device, vkDestroyBuffer); LOAD_DEVICE(device, vkGetBufferMemoryRequirements); LOAD_DEVICE(device, vkAllocateMemory); LOAD_DEVICE(device, vkFreeMemory); LOAD_DEVICE(device, vkBindBufferMemory); LOAD_DEVICE(device, vkMapMemory); LOAD_DEVICE(device, vkUnmapMemory); LOAD_DEVICE(device, vkFlushMappedMemoryRanges); LOAD_DEVICE(device, vkInvalidateMappedMemoryRanges); LOAD_DEVICE(device, vkCreateCommandPool); LOAD_DEVICE(device, vkDestroyCommandPool); LOAD_DEVICE(device, vkResetCommandPool); LOAD_DEVICE(device, vkCmdExecuteCommands); LOAD_DEVICE(device, vkAllocateCommandBuffers); LOAD_DEVICE(device, vkBeginCommandBuffer); LOAD_DEVICE(device, vkEndCommandBuffer); LOAD_DEVICE(device, vkCmdPipelineBarrier); LOAD_DEVICE(device, vkCmdCopyBufferToImage); LOAD_DEVICE(device, vkCmdCopyImage); LOAD_DEVICE(device, vkCmdCopyImageToBuffer); LOAD_DEVICE(device, vkCmdBlitImage); LOAD_DEVICE(device, vkCreateFence); LOAD_DEVICE(device, vkDestroyFence); LOAD_DEVICE(device, vkQueueSubmit); LOAD_DEVICE(device, vkWaitForFences); LOAD_DEVICE(device, vkDeviceWaitIdle);
    NEED(vkDestroyDevice); NEED(vkGetDeviceQueue); NEED(vkCreateImage); NEED(vkDestroyImage); NEED(vkGetImageMemoryRequirements); NEED(vkBindImageMemory); NEED(vkCreateBuffer); NEED(vkDestroyBuffer); NEED(vkGetBufferMemoryRequirements); NEED(vkAllocateMemory); NEED(vkFreeMemory); NEED(vkBindBufferMemory); NEED(vkMapMemory); NEED(vkUnmapMemory); NEED(vkFlushMappedMemoryRanges); NEED(vkInvalidateMappedMemoryRanges); NEED(vkCreateCommandPool); NEED(vkDestroyCommandPool); NEED(vkResetCommandPool); NEED(vkCmdExecuteCommands); NEED(vkAllocateCommandBuffers); NEED(vkBeginCommandBuffer); NEED(vkEndCommandBuffer); NEED(vkCmdPipelineBarrier); NEED(vkCmdCopyBufferToImage); NEED(vkCmdCopyImage); NEED(vkCmdCopyImageToBuffer); NEED(vkCmdBlitImage); NEED(vkCreateFence); NEED(vkDestroyFence); NEED(vkQueueSubmit); NEED(vkWaitForFences); NEED(vkDeviceWaitIdle);
    p_vkGetDeviceQueue(device, family, 0, &queue); if (!queue) { ERR("FAIL: vkGetDeviceQueue returned VK_NULL_HANDLE.\n"); status = 8; first_failure = "vkGetDeviceQueue"; goto cleanup; }
    for (i = 0; i < cfg.iterations; ++i) { unsigned c; for (c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) if (!run_case(device, queue, family, &memory_props, &cfg, (unsigned)(i + 1), &cases[c])) { status = 9; first_failure = "blit case failure"; goto cleanup; } }
    status = 0;
cleanup:
    if (device && p_vkDeviceWaitIdle) {
        VkResult idle_result;
        LOG("CALL: vkDeviceWaitIdle(final cleanup)\n");
        idle_result = p_vkDeviceWaitIdle(device);
        if (!checked(idle_result, "vkDeviceWaitIdle(final cleanup)") && status == 0) { status = 10; first_failure = "vkDeviceWaitIdle(final cleanup)"; }
    }
    if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL);
    free(devices);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    if (status) ERR("FINAL: FAIL status=%d; FIRST_FAILURE: %s\n", status, first_failure ? first_failure : "unspecified");
    else LOG("FINAL: PASS status=0; 3 exact raw vkCmdBlitImage cases per iteration completed.\n");
    return status;
}
