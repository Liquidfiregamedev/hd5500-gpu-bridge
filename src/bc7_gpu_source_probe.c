/*
 * Focused BC7 GPU-source diagnostic for the RimWorld D3D11 Vulkan bridge.
 *
 * This is deliberately distinct from bc7_matrix_probe.c.  It tests the path
 * that matters for ObjectMemory::lock: vkCmdCopyBufferToImage reads BC7 bytes
 * from a DEFAULT-like, GPU-only buffer, not from a mapped upload allocation.
 *
 * Per format (BC7_UNORM_BLOCK and BC7_SRGB_BLOCK), exactly 816 compressed
 * bytes cover all five mips of a 24x24 image.  The fixed offsets are:
 *   mip0=0 (576), mip1=576 (144), mip2=720 (64), mip3=784 (16), mip4=800 (16).
 *
 * CPU mapping is intentionally limited to host upload staging and host
 * readback allocations.  gpu_source_mem is Type 0 and explicitly rejected if
 * HostVisible; it is never an argument to vkMapMemory.
 */
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define INTEL_VENDOR_ID 0x8086u
#define ARRAY_LEN(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define ERR(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

/* The exact 24x24 chain layout: 6x6, 3x3, 2x2, 1x1, 1x1 BC7 blocks. */
static const VkDeviceSize g_mip_offsets[5] = { 0u, 576u, 720u, 784u, 800u };
static const VkDeviceSize g_mip_bytes[5] = { 576u, 144u, 64u, 16u, 16u };
#define BC7_PAYLOAD_BYTES ((VkDeviceSize)816u)

typedef struct FormatCase { const char *name; VkFormat format; } FormatCase;
static const FormatCase g_formats[] = {
    { "BC7_UNORM_BLOCK", VK_FORMAT_BC7_UNORM_BLOCK },
    { "BC7_SRGB_BLOCK", VK_FORMAT_BC7_SRGB_BLOCK }
};

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkEnumerateInstanceVersion p_vkEnumerateInstanceVersion;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkDestroyInstance p_vkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;

static PFN_vkDestroyDevice p_vkDestroyDevice;
static PFN_vkGetDeviceQueue p_vkGetDeviceQueue;
static PFN_vkCreateImage p_vkCreateImage;
static PFN_vkDestroyImage p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindImageMemory p_vkBindImageMemory;
static PFN_vkCreateBuffer p_vkCreateBuffer;
static PFN_vkDestroyBuffer p_vkDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements;
static PFN_vkBindBufferMemory p_vkBindBufferMemory;
static PFN_vkMapMemory p_vkMapMemory;
static PFN_vkUnmapMemory p_vkUnmapMemory;
static PFN_vkFlushMappedMemoryRanges p_vkFlushMappedMemoryRanges;
static PFN_vkInvalidateMappedMemoryRanges p_vkInvalidateMappedMemoryRanges;
static PFN_vkCreateCommandPool p_vkCreateCommandPool;
static PFN_vkDestroyCommandPool p_vkDestroyCommandPool;
static PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
static PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
static PFN_vkCmdCopyBuffer p_vkCmdCopyBuffer;
static PFN_vkCmdCopyBufferToImage p_vkCmdCopyBufferToImage;
static PFN_vkCmdCopyImageToBuffer p_vkCmdCopyImageToBuffer;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkDestroyFence p_vkDestroyFence;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkWaitForFences p_vkWaitForFences;
static PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle;

#define LOAD_GLOBAL(n) p_##n = (PFN_##n)gipa(VK_NULL_HANDLE, #n)
#define LOAD_INSTANCE(i,n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d,n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)

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
static int is_absolute_windows_path(const wchar_t *p) {
    return p && ((p[0] == L'\\' && p[1] == L'\\') ||
        (iswalpha((wint_t)p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')));
}
static int ci_contains(const char *text, const char *part) {
    size_t n = strlen(part);
    if (!n) return 1;
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
static const char *device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "OTHER";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "DISCRETE_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "VIRTUAL_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
    default: return "unknown";
    }
}
static int find_transfer_queue(VkPhysicalDevice physical, uint32_t *out) {
    uint32_t n = 0, i; VkQueueFamilyProperties *p;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, NULL);
    if (!n || !(p = (VkQueueFamilyProperties *)calloc(n, sizeof(*p)))) return 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, p);
    for (i = 0; i < n; ++i) {
        LOG("QUEUE[%u]: flags=0x%X count=%u\n", i, p[i].queueFlags, p[i].queueCount);
        if (p[i].queueCount && (p[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) { *out = i; free(p); return 1; }
    }
    free(p); return 0;
}
static int choose_host_coherent_type(uint32_t bits, const VkPhysicalDeviceMemoryProperties *mp) {
    uint32_t i;
    for (i = 0; i < mp->memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return (int)i;
    return -1;
}
/* This diagnostic intentionally permits no replacement type: GPU source is Type 0 only. */
static int require_gpu_only_type0(uint32_t bits, const VkPhysicalDeviceMemoryProperties *mp) {
    VkMemoryPropertyFlags flags;
    if (mp->memoryTypeCount == 0 || !(bits & 1u)) return 0;
    flags = mp->memoryTypes[0].propertyFlags;
    return (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
           (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0;
}
static void fill_valid_bc7_payload(uint8_t *dst) {
    VkDeviceSize off; uint32_t level, j;
    /* Retains the base probe's deterministic per-mip mode-6 pattern.  Mode 6
       has low six mode bits zero and bit 6 one; all remaining endpoint/pbit/
       index bits are legal. */
    for (level = 0; level < 5u; ++level) {
        for (off = 0; off < g_mip_bytes[level]; off += 16u) {
            dst[g_mip_offsets[level] + off] = 0x40u;
            for (j = 1; j < 16u; ++j)
                dst[g_mip_offsets[level] + off + j] = (uint8_t)(0x35u ^
                    (level * 0x29u) ^ ((uint32_t)(off / 16u) * 0x13u) ^ (j * 0x0Du));
        }
    }
}
static VkImageCreateInfo make_image_info(VkFormat format) {
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format;
    ii.extent.width = 24u; ii.extent.height = 24u; ii.extent.depth = 1u;
    ii.mipLevels = 5u; ii.arrayLayers = 1u; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return ii;
}

static VkResult allocate_host_buffer(VkDevice device, const VkPhysicalDeviceMemoryProperties *mp,
    VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory, const char *name) {
    VkBufferCreateInfo bi; VkMemoryRequirements req; VkMemoryAllocateInfo ai; VkResult r; int type;
    memset(&bi, 0, sizeof(bi)); bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = BC7_PAYLOAD_BYTES; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = p_vkCreateBuffer(device, &bi, NULL, buffer);
    LOG("BUFFER_CREATE_RESULT: name=%s bytes=816 usage=0x%X => %s (%d)\n", name, usage, result_name(r), (int)r);
    if (r != VK_SUCCESS) return r;
    p_vkGetBufferMemoryRequirements(device, *buffer, &req);
    type = choose_host_coherent_type(req.memoryTypeBits, mp);
    LOG("BUFFER_REQUIREMENTS: name=%s reqBytes=%llu alignment=%llu typeBits=0x%08X hostCoherentType=%d\n",
        name, (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits, type);
    if (type < 0) return VK_ERROR_FEATURE_NOT_PRESENT;
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)type;
    r = p_vkAllocateMemory(device, &ai, NULL, memory);
    LOG("BUFFER_ALLOC_RESULT: name=%s type=%d flags=0x%X => %s (%d)\n", name, type,
        mp->memoryTypes[type].propertyFlags, result_name(r), (int)r);
    if (r != VK_SUCCESS) return r;
    r = p_vkBindBufferMemory(device, *buffer, *memory, 0);
    LOG("BUFFER_BIND_RESULT: name=%s offset=0 => %s (%d)\n", name, result_name(r), (int)r);
    return r;
}

/* Creates the second/default source buffer.  No caller may map *memory. */
static VkResult allocate_gpu_only_source_type0(VkDevice device, const VkPhysicalDeviceMemoryProperties *mp,
    VkBuffer *buffer, VkDeviceMemory *memory) {
    VkBufferCreateInfo bi; VkMemoryRequirements req; VkMemoryAllocateInfo ai; VkResult r;
    memset(&bi, 0, sizeof(bi)); bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = BC7_PAYLOAD_BYTES;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = p_vkCreateBuffer(device, &bi, NULL, buffer);
    LOG("GPU_SOURCE_CREATE_RESULT: name=DEFAULT_GPU_ONLY_SOURCE bytes=816 usage=TRANSFER_DST|TRANSFER_SRC(0x%X) => %s (%d)\n",
        bi.usage, result_name(r), (int)r);
    if (r != VK_SUCCESS) return r;
    p_vkGetBufferMemoryRequirements(device, *buffer, &req);
    LOG("GPU_SOURCE_REQUIREMENTS: typeBits=0x%08X reqBytes=%llu alignment=%llu requestedType=0 flags=0x%X\n",
        req.memoryTypeBits, (unsigned long long)req.size, (unsigned long long)req.alignment,
        mp->memoryTypeCount ? mp->memoryTypes[0].propertyFlags : 0u);
    if (!require_gpu_only_type0(req.memoryTypeBits, mp)) {
        ERR("GPU_SOURCE_CONTRACT_FAILURE: Type0 must be buffer-compatible DEVICE_LOCAL and NOT HOST_VISIBLE; no alternate or HostVisible type is allowed.\n");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = 0u;
    r = p_vkAllocateMemory(device, &ai, NULL, memory);
    LOG("GPU_SOURCE_ALLOC_RESULT: type=0 flags=0x%X hostVisible=0 => %s (%d)\n",
        mp->memoryTypes[0].propertyFlags, result_name(r), (int)r);
    if (r != VK_SUCCESS) return r;
    r = p_vkBindBufferMemory(device, *buffer, *memory, 0);
    LOG("GPU_SOURCE_BIND_RESULT: buffer=DEFAULT_GPU_ONLY_SOURCE type=0 offset=0 => %s (%d)\n", result_name(r), (int)r);
    return r;
}

static int run_one_format(VkDevice device, VkQueue queue, uint32_t family,
    const VkPhysicalDeviceMemoryProperties *mp, const FormatCase *format) {
    VkBuffer host_upload = VK_NULL_HANDLE, gpu_source = VK_NULL_HANDLE, host_readback = VK_NULL_HANDLE;
    VkDeviceMemory host_upload_mem = VK_NULL_HANDLE, gpu_source_mem = VK_NULL_HANDLE, host_readback_mem = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE; VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkMemoryRequirements req; VkMemoryAllocateInfo ai; VkResult r = VK_SUCCESS; int image_type, failed = 1;
    uint8_t expected[816]; uint8_t *mapped = NULL; uint32_t level;
    VkBufferImageCopy regions[5]; const uint32_t widths[5] = {24u, 12u, 6u, 3u, 1u};

    memset(&req, 0, sizeof(req)); memset(regions, 0, sizeof(regions));
    fill_valid_bc7_payload(expected);
    for (level = 0; level < 5u; ++level) {
        regions[level].bufferOffset = g_mip_offsets[level];
        regions[level].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[level].imageSubresource.mipLevel = level;
        regions[level].imageSubresource.baseArrayLayer = 0u;
        regions[level].imageSubresource.layerCount = 1u;
        regions[level].imageExtent.width = widths[level]; regions[level].imageExtent.height = widths[level]; regions[level].imageExtent.depth = 1u;
        LOG("MIP_LAYOUT: format=%s mip=%u offset=%llu bytes=%llu extent=%ux%u validBC7Mode6Prefix=40\n",
            format->name, level, (unsigned long long)g_mip_offsets[level],
            (unsigned long long)g_mip_bytes[level], widths[level], widths[level]);
    }
    LOG("FORMAT_BEGIN: format=%s payloadBytes=816 sourcePath=HOST_STAGING --vkCmdCopyBuffer--> GPU_ONLY_TYPE0 --vkCmdCopyBufferToImage--> IMAGE --vkCmdCopyImageToBuffer--> HOST_READBACK\n", format->name);

    r = allocate_host_buffer(device, mp, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &host_upload, &host_upload_mem, "HOST_UPLOAD_STAGING");
    if (r != VK_SUCCESS) goto done;
    r = allocate_gpu_only_source_type0(device, mp, &gpu_source, &gpu_source_mem);
    if (r != VK_SUCCESS) goto done;
    r = allocate_host_buffer(device, mp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &host_readback, &host_readback_mem, "HOST_READBACK");
    if (r != VK_SUCCESS) goto done;

    /* CPU writes only the host staging allocation; gpu_source_mem has no map call anywhere. */
    r = p_vkMapMemory(device, host_upload_mem, 0, BC7_PAYLOAD_BYTES, 0, (void **)&mapped);
    LOG("HOST_UPLOAD_MAP_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r);
    if (r != VK_SUCCESS) goto done;
    memcpy(mapped, expected, sizeof(expected));
    { VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, host_upload_mem, 0, VK_WHOLE_SIZE };
      r = p_vkFlushMappedMemoryRanges(device, 1u, &mr);
      LOG("HOST_UPLOAD_FLUSH_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r); }
    p_vkUnmapMemory(device, host_upload_mem); mapped = NULL;
    if (r != VK_SUCCESS) goto done;

    {
        VkImageCreateInfo ii = make_image_info(format->format);
        r = p_vkCreateImage(device, &ii, NULL, &image);
        LOG("IMAGE_CREATE_RESULT: format=%s 24x24 mips=5 => %s (%d)\n", format->name, result_name(r), (int)r);
        if (r != VK_SUCCESS) goto done;
        p_vkGetImageMemoryRequirements(device, image, &req);
        image_type = -1;
        for (level = 0; level < mp->memoryTypeCount; ++level)
            if ((req.memoryTypeBits & (1u << level)) && (mp->memoryTypes[level].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { image_type = (int)level; break; }
        LOG("IMAGE_REQUIREMENTS: format=%s typeBits=0x%08X deviceLocalType=%d\n", format->name, req.memoryTypeBits, image_type);
        if (image_type < 0) { r = VK_ERROR_FEATURE_NOT_PRESENT; goto done; }
        memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)image_type;
        r = p_vkAllocateMemory(device, &ai, NULL, &image_mem);
        LOG("IMAGE_ALLOC_RESULT: format=%s type=%d => %s (%d)\n", format->name, image_type, result_name(r), (int)r);
        if (r != VK_SUCCESS) goto done;
        r = p_vkBindImageMemory(device, image, image_mem, 0);
        LOG("IMAGE_BIND_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r);
        if (r != VK_SUCCESS) goto done;
    }

    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, family };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1u };
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0u };
        VkSubmitInfo submit; VkBufferCopy source_copy = { 0u, 0u, BC7_PAYLOAD_BYTES };
        VkBufferMemoryBarrier source_ready; VkImageMemoryBarrier image_to_dst, image_to_src; VkBufferMemoryBarrier readback_ready;
        VkImageSubresourceRange all_mips;
        r = p_vkCreateCommandPool(device, &pi, NULL, &pool);
        LOG("CMDPOOL_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r); if (r != VK_SUCCESS) goto done;
        cai.commandPool = pool; r = p_vkAllocateCommandBuffers(device, &cai, &cmd);
        LOG("CMDBUF_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r); if (r != VK_SUCCESS) goto done;
        r = p_vkBeginCommandBuffer(cmd, &begin);
        LOG("BEGIN_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r); if (r != VK_SUCCESS) goto done;

        /* Actual GPU delivery: this is the sole producer of DEFAULT_GPU_ONLY_SOURCE. */
        p_vkCmdCopyBuffer(cmd, host_upload, gpu_source, 1u, &source_copy);
        LOG("RECORDED: vkCmdCopyBuffer HOST_UPLOAD_STAGING -> DEFAULT_GPU_ONLY_SOURCE bytes=816\n");
        memset(&source_ready, 0, sizeof(source_ready)); source_ready.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        source_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; source_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        source_ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; source_ready.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        source_ready.buffer = gpu_source; source_ready.offset = 0u; source_ready.size = BC7_PAYLOAD_BYTES;
        p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 1u, &source_ready, 0u, NULL);
        LOG("RECORDED: barrier DEFAULT_GPU_ONLY_SOURCE TRANSFER_WRITE -> TRANSFER_READ\n");

        memset(&all_mips, 0, sizeof(all_mips)); all_mips.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; all_mips.levelCount = 5u; all_mips.layerCount = 1u;
        memset(&image_to_dst, 0, sizeof(image_to_dst)); image_to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        image_to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; image_to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image_to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; image_to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_to_dst.image = image; image_to_dst.subresourceRange = all_mips; image_to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &image_to_dst);

        /* Crucially this reads gpu_source, never host_upload. */
        p_vkCmdCopyBufferToImage(cmd, gpu_source, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 5u, regions);
        LOG("RECORDED: vkCmdCopyBufferToImage source=DEFAULT_GPU_ONLY_SOURCE regions=5\n");
        memset(&image_to_src, 0, sizeof(image_to_src)); image_to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        image_to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; image_to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image_to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; image_to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_to_src.image = image; image_to_src.subresourceRange = all_mips;
        image_to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; image_to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &image_to_src);
        p_vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host_readback, 5u, regions);
        memset(&readback_ready, 0, sizeof(readback_ready)); readback_ready.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        readback_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; readback_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readback_ready.buffer = host_readback; readback_ready.offset = 0u; readback_ready.size = BC7_PAYLOAD_BYTES;
        readback_ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; readback_ready.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0u, 0u, NULL, 1u, &readback_ready, 0u, NULL);
        r = p_vkEndCommandBuffer(cmd);
        LOG("END_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r); if (r != VK_SUCCESS) goto done;
        r = p_vkCreateFence(device, &fi, NULL, &fence);
        LOG("FENCE_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r); if (r != VK_SUCCESS) goto done;
        memset(&submit, 0, sizeof(submit)); submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1u; submit.pCommandBuffers = &cmd;
        r = p_vkQueueSubmit(queue, 1u, &submit, fence);
        LOG("SUBMIT_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r); if (r != VK_SUCCESS) goto done;
        r = p_vkWaitForFences(device, 1u, &fence, VK_TRUE, UINT64_C(15000000000));
        LOG("WAIT_RESULT: format=%s timeoutNs=15000000000 => %s (%d)\n", format->name, result_name(r), (int)r); if (r != VK_SUCCESS) goto done;
    }

    /* CPU reads only host_readback_mem.  Exact expected bytes are regenerated, not copied from output. */
    r = p_vkMapMemory(device, host_readback_mem, 0, BC7_PAYLOAD_BYTES, 0, (void **)&mapped);
    LOG("HOST_READBACK_MAP_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r);
    if (r != VK_SUCCESS) goto done;
    { VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, host_readback_mem, 0, VK_WHOLE_SIZE };
      r = p_vkInvalidateMappedMemoryRanges(device, 1u, &mr);
      LOG("HOST_READBACK_INVALIDATE_RESULT: format=%s => %s (%d)\n", format->name, result_name(r), (int)r); }
    if (r != VK_SUCCESS) goto done;
    for (level = 0; level < 5u; ++level) {
        int exact = memcmp(mapped + g_mip_offsets[level], expected + g_mip_offsets[level], (size_t)g_mip_bytes[level]) == 0;
        LOG("READBACK_MIP_RESULT: format=%s mip=%u offset=%llu bytes=%llu => %s\n", format->name, level,
            (unsigned long long)g_mip_offsets[level], (unsigned long long)g_mip_bytes[level], exact ? "EXACT_MATCH" : "MISMATCH");
        if (!exact) goto done;
    }
    failed = 0;

done:
    if (mapped && host_readback_mem) p_vkUnmapMemory(device, host_readback_mem);
    if (fence) p_vkDestroyFence(device, fence, NULL);
    if (pool) p_vkDestroyCommandPool(device, pool, NULL);
    if (image) p_vkDestroyImage(device, image, NULL);
    if (host_readback) p_vkDestroyBuffer(device, host_readback, NULL);
    if (gpu_source) p_vkDestroyBuffer(device, gpu_source, NULL);
    if (host_upload) p_vkDestroyBuffer(device, host_upload, NULL);
    if (image_mem) p_vkFreeMemory(device, image_mem, NULL);
    if (host_readback_mem) p_vkFreeMemory(device, host_readback_mem, NULL);
    if (gpu_source_mem) p_vkFreeMemory(device, gpu_source_mem, NULL);
    if (host_upload_mem) p_vkFreeMemory(device, host_upload_mem, NULL);
    if (failed) ERR("FORMAT_FINAL: format=%s => FAIL; no successful fallback result is accepted.\n", format->name);
    else LOG("FORMAT_FINAL: format=%s => PASS exact all five mips through GPU-only Type0 source.\n", format->name);
    return failed;
}

static void usage(const wchar_t *exe) {
    LOG("Usage:\n  %ls --loader C:\\absolute\\vulkan-1.dll --icd C:\\absolute\\ashesD3D11Renderer_icd.json [--allow-non-intel-test]\n", exe);
    LOG("Default policy requires a non-software Intel adapter. --allow-non-intel-test is only for the sandbox integration run.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL; int allow_non_intel = 0, i, status = 1, failures = 0;
    HMODULE loader = NULL; union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } l;
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice *list = NULL, physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mp; uint32_t physical_count = 0, family = UINT32_MAX;
    VkDevice device = VK_NULL_HANDLE; VkQueue queue = VK_NULL_HANDLE; const char *first_failure = NULL;
    memset(&props, 0, sizeof(props)); memset(&mp, 0, sizeof(mp));
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { usage(argv[0]); return 0; }
        else { ERR("FAIL: unrecognized or incomplete argument: %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!loader_path || !icd_path || !is_absolute_windows_path(loader_path) || !is_absolute_windows_path(icd_path)) { ERR("FAIL: absolute --loader and --icd are mandatory.\n"); return 2; }
    if (GetFileAttributesW(loader_path) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(icd_path) == INVALID_FILE_ATTRIBUTES) { ERR("FAIL: supplied loader or ICD path is inaccessible (GetLastError=%lu).\n", GetLastError()); return 2; }
    if (GetModuleHandleW(L"vulkan-1.dll")) { ERR("FAIL: vulkan-1.dll already loaded; refusing ambiguous loader selection.\n"); return 3; }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path) || !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) { ERR("FAIL: could not set isolated ICD variables.\n"); return 3; }
    LOG("RimWorld focused BC7 GPU-only source probe\nLOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    LOG("CONTRACT: Type0 only, DEVICE_LOCAL required, HOST_VISIBLE forbidden, no vkMapMemory(gpu_source_mem), actual vkCmdCopyBuffer producer, then GPU source in vkCmdCopyBufferToImage.\n");
    if (allow_non_intel) LOG("WARNING: --allow-non-intel-test relaxes only the Intel vendor gate for sandbox integration; CPU/software adapters remain forbidden and this is not hardware validation.\n");
    loader = LoadLibraryW(loader_path); if (!loader) { status = 4; first_failure = "LoadLibraryW(loader)"; goto cleanup; }
    l.raw = GetProcAddress(loader, "vkGetInstanceProcAddr"); gipa = l.typed; if (!gipa) { status = 4; first_failure = "vkGetInstanceProcAddr"; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion); LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties); LOAD_GLOBAL(vkCreateInstance);
    if (!p_vkEnumerateInstanceExtensionProperties || !p_vkCreateInstance) { status = 20; first_failure = "global entrypoint"; goto cleanup; }
    if (p_vkEnumerateInstanceVersion) { uint32_t v = VK_API_VERSION_1_0; VkResult r = p_vkEnumerateInstanceVersion(&v); LOG("RESULT: vkEnumerateInstanceVersion => %s (%d), max=%u.%u.%u\n", result_name(r), (int)r, VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v)); if (r != VK_SUCCESS) { status = 21; first_failure = "vkEnumerateInstanceVersion"; goto cleanup; } }
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "RimWorld BC7 GPU-only source", 1u, "standalone", 1u, VK_API_VERSION_1_0 };
        VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0u, &app, 0u, NULL, 0u, NULL };
        VkResult r = p_vkCreateInstance(&ci, NULL, &instance);
        LOG("RESULT: vkCreateInstance(api=1.0) => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateInstance"; goto cleanup; }
    }
    LOAD_INSTANCE(instance, vkDestroyInstance); LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices); LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties); LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties); LOAD_INSTANCE(instance, vkCreateDevice); LOAD_INSTANCE(instance, vkGetDeviceProcAddr);
    if (!p_vkDestroyInstance || !p_vkEnumeratePhysicalDevices || !p_vkGetPhysicalDeviceProperties || !p_vkGetPhysicalDeviceMemoryProperties || !p_vkGetPhysicalDeviceQueueFamilyProperties || !p_vkEnumerateDeviceExtensionProperties || !p_vkCreateDevice || !p_vkGetDeviceProcAddr) { status = 20; first_failure = "instance entrypoint"; goto cleanup; }
    { VkResult r = p_vkEnumeratePhysicalDevices(instance, &physical_count, NULL); LOG("RESULT: vkEnumeratePhysicalDevices(count) => %s (%d), count=%u\n", result_name(r), (int)r, physical_count); if (r != VK_SUCCESS || !physical_count) { status = 5; first_failure = "physical device enumeration"; goto cleanup; } }
    if (!(list = (VkPhysicalDevice *)calloc(physical_count, sizeof(*list)))) { status = 5; first_failure = "physical device list allocation"; goto cleanup; }
    { VkResult r = p_vkEnumeratePhysicalDevices(instance, &physical_count, list); if (r != VK_SUCCESS) { status = 5; first_failure = "physical device enumeration list"; goto cleanup; } }
    for (i = 0; i < (int)physical_count; ++i) {
        VkPhysicalDeviceProperties q; uint32_t candidate;
        p_vkGetPhysicalDeviceProperties(list[i], &q);
        LOG("ADAPTER[%d]: name=%s vendor=0x%04X device=0x%04X type=%s%s\n", i, q.deviceName, q.vendorID, q.deviceID, device_type_name(q.deviceType), looks_software(&q) ? " [software-like]" : "");
        if (looks_software(&q)) { LOG("ADAPTER_REJECTED: CPU/software fallback is never eligible.\n"); continue; }
        if (!allow_non_intel && q.vendorID != INTEL_VENDOR_ID) continue;
        if (!physical && find_transfer_queue(list[i], &candidate)) { physical = list[i]; family = candidate; props = q; }
    }
    if (!physical) { status = 6; first_failure = "eligible adapter selection (default is non-software Intel only)"; goto cleanup; }
    LOG("SELECTED: %s vendor=0x%04X queueFamily=%u policy=%s\n", props.deviceName, props.vendorID, family, allow_non_intel ? "sandbox-non-Intel-hardware-allowed" : "Intel-non-software-required");
    p_vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (i = 0; i < (int)mp.memoryTypeCount; ++i) LOG("MEMORY_TYPE[%d]: heap=%u flags=0x%X\n", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags);
    {
        float priority = 1.0f; VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0u, family, 1u, &priority };
        VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0u, 1u, &qi, 0u, NULL, 0u, NULL, NULL };
        const char *subset = "VK_KHR_portability_subset"; VkResult r;
        uint32_t n = 0; VkExtensionProperties *ep = NULL;
        if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &n, NULL) == VK_SUCCESS && n && (ep = (VkExtensionProperties *)calloc(n, sizeof(*ep))) && p_vkEnumerateDeviceExtensionProperties(physical, NULL, &n, ep) == VK_SUCCESS) {
            for (i = 0; i < (int)n; ++i) if (!strcmp(ep[i].extensionName, subset)) { ci.enabledExtensionCount = 1u; ci.ppEnabledExtensionNames = &subset; break; }
        }
        free(ep); r = p_vkCreateDevice(physical, &ci, NULL, &device);
        LOG("RESULT: vkCreateDevice => %s (%d)\n", result_name(r), (int)r); if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateDevice"; goto cleanup; }
    }
    LOAD_DEVICE(device, vkDestroyDevice); LOAD_DEVICE(device, vkGetDeviceQueue); LOAD_DEVICE(device, vkCreateImage); LOAD_DEVICE(device, vkDestroyImage); LOAD_DEVICE(device, vkGetImageMemoryRequirements); LOAD_DEVICE(device, vkAllocateMemory); LOAD_DEVICE(device, vkFreeMemory); LOAD_DEVICE(device, vkBindImageMemory); LOAD_DEVICE(device, vkCreateBuffer); LOAD_DEVICE(device, vkDestroyBuffer); LOAD_DEVICE(device, vkGetBufferMemoryRequirements); LOAD_DEVICE(device, vkBindBufferMemory); LOAD_DEVICE(device, vkMapMemory); LOAD_DEVICE(device, vkUnmapMemory); LOAD_DEVICE(device, vkFlushMappedMemoryRanges); LOAD_DEVICE(device, vkInvalidateMappedMemoryRanges); LOAD_DEVICE(device, vkCreateCommandPool); LOAD_DEVICE(device, vkDestroyCommandPool); LOAD_DEVICE(device, vkAllocateCommandBuffers); LOAD_DEVICE(device, vkBeginCommandBuffer); LOAD_DEVICE(device, vkEndCommandBuffer); LOAD_DEVICE(device, vkCmdPipelineBarrier); LOAD_DEVICE(device, vkCmdCopyBuffer); LOAD_DEVICE(device, vkCmdCopyBufferToImage); LOAD_DEVICE(device, vkCmdCopyImageToBuffer); LOAD_DEVICE(device, vkCreateFence); LOAD_DEVICE(device, vkDestroyFence); LOAD_DEVICE(device, vkQueueSubmit); LOAD_DEVICE(device, vkWaitForFences); LOAD_DEVICE(device, vkDeviceWaitIdle);
    if (!p_vkDestroyDevice || !p_vkGetDeviceQueue || !p_vkCreateImage || !p_vkDestroyImage || !p_vkGetImageMemoryRequirements || !p_vkAllocateMemory || !p_vkFreeMemory || !p_vkBindImageMemory || !p_vkCreateBuffer || !p_vkDestroyBuffer || !p_vkGetBufferMemoryRequirements || !p_vkBindBufferMemory || !p_vkMapMemory || !p_vkUnmapMemory || !p_vkFlushMappedMemoryRanges || !p_vkInvalidateMappedMemoryRanges || !p_vkCreateCommandPool || !p_vkDestroyCommandPool || !p_vkAllocateCommandBuffers || !p_vkBeginCommandBuffer || !p_vkEndCommandBuffer || !p_vkCmdPipelineBarrier || !p_vkCmdCopyBuffer || !p_vkCmdCopyBufferToImage || !p_vkCmdCopyImageToBuffer || !p_vkCreateFence || !p_vkDestroyFence || !p_vkQueueSubmit || !p_vkWaitForFences || !p_vkDeviceWaitIdle) { status = 20; first_failure = "device entrypoint"; goto cleanup; }
    p_vkGetDeviceQueue(device, family, 0u, &queue); if (!queue) { status = 7; first_failure = "vkGetDeviceQueue"; goto cleanup; }
    for (i = 0; i < (int)ARRAY_LEN(g_formats); ++i) failures += run_one_format(device, queue, family, &mp, &g_formats[i]);
    { VkResult r = p_vkDeviceWaitIdle(device); LOG("RESULT: vkDeviceWaitIdle => %s (%d)\n", result_name(r), (int)r); if (r != VK_SUCCESS) { status = 21; first_failure = "vkDeviceWaitIdle"; } }
    if (status == 1) { if (failures) { status = 11; first_failure = "focused GPU-only BC7 source transfer failure"; } else status = 0; }
cleanup:
    if (device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(device);
    if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL);
    free(list);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    LOG("GPU_SOURCE_BC7_SUMMARY: formats=2 payloadBytesPerFormat=816 offsets=0,576,720,784,800 failures=%d\n", failures);
    if (status) ERR("FINAL: FAIL status=%d; FIRST_FAILURE: %s\n", status, first_failure ? first_failure : "unspecified");
    else LOG("FINAL: PASS status=0; both BC7 formats exactly traversed actual GPU copy into Type0 GPU-only source and all five image mips round-tripped.\n");
    return status;
}
