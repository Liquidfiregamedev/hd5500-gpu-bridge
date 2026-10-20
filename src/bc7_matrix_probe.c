/*
 * Raw Vulkan BC7 capability/resource diagnostic for the RimWorld D3D11 bridge.
 *
 * Deliberately loader/ICD isolated: no Vulkan import library is linked and the
 * caller supplies absolute loader and manifest paths.  This is not a game or
 * an Intel-hardware claim; --allow-non-intel-test exists only for this Wine
 * sandbox integration run.
 *
 * Matrix (each descriptor gets a fresh image/allocation; no parent pools):
 *   BC7_UNORM_BLOCK / BC7_SRGB_BLOCK
 *   SAMPLED / SAMPLED|TRANSFER_SRC|TRANSFER_DST
 *   1x1 (1 mip), 2x2 (2), 24x24 (5), 32x32 (6), 64x64 (7),
 *   8000x5000 (13).  Each image is bound once with a compatible device-local
 *   type and once with a compatible HOST_VISIBLE|HOST_COHERENT type when
 *   advertised.  Every successful bind also creates both a full SRV-style
 *   VkImageView (base=0) and a tail-mip VkImageView.
 *
 * Exact transfer test: valid BC7 mode-6 blocks (LSB unary mode prefix 0000001,
 * byte 0 == 0x40) are copied into and out of a 24x24, 5-mip image.  The test
 * compares all five individual tail-inclusive mip byte ranges, not decoded
 * pixels and not a substitute format.
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
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define ERR(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#define ARRAY_LEN(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkEnumerateInstanceVersion p_vkEnumerateInstanceVersion;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkDestroyInstance p_vkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceImageFormatProperties p_vkGetPhysicalDeviceImageFormatProperties;
static PFN_vkGetPhysicalDeviceFormatProperties p_vkGetPhysicalDeviceFormatProperties;
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkCreateDebugUtilsMessengerEXT p_vkCreateDebugUtilsMessengerEXT;
static PFN_vkDestroyDebugUtilsMessengerEXT p_vkDestroyDebugUtilsMessengerEXT;

static PFN_vkDestroyDevice p_vkDestroyDevice;
static PFN_vkGetDeviceQueue p_vkGetDeviceQueue;
static PFN_vkCreateImage p_vkCreateImage;
static PFN_vkDestroyImage p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindImageMemory p_vkBindImageMemory;
static PFN_vkCreateImageView p_vkCreateImageView;
static PFN_vkDestroyImageView p_vkDestroyImageView;
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

static LONG g_debug_total;
static LONG g_debug_errors;
static LONG g_debug_bad_text;
static const char *g_current_operation;
static const char *g_first_debug_operation;

typedef struct FormatCase { const char *name; VkFormat format; } FormatCase;
typedef struct UsageCase { const char *name; VkImageUsageFlags flags; } UsageCase;
typedef struct SizeCase { const char *name; uint32_t width, height, mips; } SizeCase;
typedef struct MatrixCounts {
    uint32_t queries, query_failures, creates, create_failures;
    uint32_t allocations, allocation_failures, binds, bind_failures;
    uint32_t full_views, full_view_failures, tail_views, tail_view_failures;
    uint32_t no_device_local, no_host_visible_coherent;
    uint32_t upload_cases, upload_failures, readback_mip_mismatches;
} MatrixCounts;

static const FormatCase g_formats[] = {
    { "BC7_UNORM_BLOCK", VK_FORMAT_BC7_UNORM_BLOCK },
    { "BC7_SRGB_BLOCK", VK_FORMAT_BC7_SRGB_BLOCK }
};
static const UsageCase g_usages[] = {
    { "SAMPLED", VK_IMAGE_USAGE_SAMPLED_BIT },
    { "SAMPLED|TRANSFER_SRC|TRANSFER_DST", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT }
};
static const SizeCase g_sizes[] = {
    { "1x1", 1u, 1u, 1u }, { "2x2", 2u, 2u, 2u },
    { "24x24", 24u, 24u, 5u }, { "32x32", 32u, 32u, 6u },
    { "64x64", 64u, 64u, 7u }, { "8000x5000", 8000u, 5000u, 13u }
};

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
static int text_is_sane(const char *s) {
    size_t i;
    if (!s) return 1;
    for (i = 0; i < 1024; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!c) return 1;
        if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f) return 0;
    }
    return 0;
}
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    LONG n = InterlockedIncrement(&g_debug_total);
    const char *id = data && data->pMessageIdName ? data->pMessageIdName : "<none>";
    const char *msg = data && data->pMessage ? data->pMessage : "<none>";
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        InterlockedIncrement(&g_debug_errors);
        if (!g_first_debug_operation) g_first_debug_operation = g_current_operation ? g_current_operation : "untagged";
    }
    if (!text_is_sane(id) || !text_is_sane(msg)) InterlockedIncrement(&g_debug_bad_text);
    LOG("DEBUG_UTILS[%ld]: severity=0x%X type=0x%X code=%d id=%s message=%s\n", n,
        (unsigned)severity, (unsigned)types, data ? data->messageIdNumber : 0, id, msg);
    return VK_FALSE;
}
static int instance_extension_present(const char *wanted) {
    uint32_t n = 0, i; VkExtensionProperties *p = NULL; int result = 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS) return 0;
    if (n && !(p = (VkExtensionProperties *)calloc(n, sizeof(*p)))) return 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &n, p) == VK_SUCCESS)
        for (i = 0; i < n; ++i) if (!strcmp(p[i].extensionName, wanted)) { result = 1; break; }
    free(p); return result;
}
static int device_extension_present(VkPhysicalDevice physical, const char *wanted) {
    uint32_t n = 0, i; VkExtensionProperties *p = NULL; int result = 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &n, NULL) != VK_SUCCESS) return 0;
    if (n && !(p = (VkExtensionProperties *)calloc(n, sizeof(*p)))) return 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &n, p) == VK_SUCCESS)
        for (i = 0; i < n; ++i) if (!strcmp(p[i].extensionName, wanted)) { result = 1; break; }
    free(p); return result;
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
static int choose_memory_type(uint32_t bits, const VkPhysicalDeviceMemoryProperties *p, VkMemoryPropertyFlags required) {
    uint32_t i;
    for (i = 0; i < p->memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (p->memoryTypes[i].propertyFlags & required) == required) return (int)i;
    return -1;
}
static VkImageCreateInfo make_image_info(VkFormat format, VkImageUsageFlags usage, const SizeCase *size) {
    VkImageCreateInfo i;
    memset(&i, 0, sizeof(i));
    i.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    i.imageType = VK_IMAGE_TYPE_2D; i.format = format;
    i.extent.width = size->width; i.extent.height = size->height; i.extent.depth = 1;
    i.mipLevels = size->mips; i.arrayLayers = 1; i.samples = VK_SAMPLE_COUNT_1_BIT;
    i.tiling = VK_IMAGE_TILING_OPTIMAL; i.usage = usage;
    i.sharingMode = VK_SHARING_MODE_EXCLUSIVE; i.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return i;
}

/* Called only after a successful bind, so it distinguishes resource from SRV/view failure. */
static int create_required_views(VkDevice device, VkImage image, VkFormat format, const SizeCase *size,
                                 const char *fname, const char *uname, const char *memname, MatrixCounts *c) {
    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo vi;
    VkResult r;
    int bad = 0;
    memset(&vi, 0, sizeof(vi)); vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseArrayLayer = 0; vi.subresourceRange.layerCount = 1;
    vi.subresourceRange.baseMipLevel = 0; vi.subresourceRange.levelCount = size->mips;
    g_current_operation = "vkCreateImageView(SRV full requested mip range)";
    r = p_vkCreateImageView(device, &vi, NULL, &view);
    g_current_operation = NULL;
    ++c->full_views;
    LOG("VIEW_RESULT: kind=SRV_FULL format=%s size=%s mips=%u usage=%s class=%s baseMip=0 levelCount=%u => %s (%d)\n",
        fname, size->name, size->mips, uname, memname, size->mips, result_name(r), (int)r);
    if (r != VK_SUCCESS) { ++c->full_view_failures; bad = 1; }
    if (view) { p_vkDestroyImageView(device, view, NULL); view = VK_NULL_HANDLE; }

    vi.subresourceRange.baseMipLevel = size->mips - 1u; vi.subresourceRange.levelCount = 1;
    g_current_operation = "vkCreateImageView(SRV exact tail mip)";
    r = p_vkCreateImageView(device, &vi, NULL, &view);
    g_current_operation = NULL;
    ++c->tail_views;
    LOG("VIEW_RESULT: kind=SRV_TAIL format=%s size=%s mips=%u usage=%s class=%s baseMip=%u levelCount=1 => %s (%d)\n",
        fname, size->name, size->mips, uname, memname, size->mips - 1u, result_name(r), (int)r);
    if (r != VK_SUCCESS) { ++c->tail_view_failures; bad = 1; }
    if (view) p_vkDestroyImageView(device, view, NULL);
    return bad;
}

/* Exactly one resource and allocation per compatible class; never share a giant parent allocation. */
static void create_bind_view_one(VkDevice device, const VkPhysicalDeviceMemoryProperties *mp,
    const FormatCase *f, const UsageCase *u, const SizeCase *s, VkMemoryPropertyFlags required,
    const char *class_name, MatrixCounts *c) {
    VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
    VkMemoryRequirements req; VkMemoryAllocateInfo ai; VkImageCreateInfo ii = make_image_info(f->format, u->flags, s);
    VkResult r; int type;
    memset(&req, 0, sizeof(req)); memset(&ai, 0, sizeof(ai));
    ++c->creates;
    LOG("CREATE_ATTEMPT: format=%s(%d) size=%s extent=%ux%u mips=%u usage=%s class=%s requiredFlags=0x%X\n",
        f->name, (int)f->format, s->name, s->width, s->height, s->mips, u->name, class_name, required);
    g_current_operation = "vkCreateImage(BC7 matrix)";
    r = p_vkCreateImage(device, &ii, NULL, &image);
    g_current_operation = NULL;
    LOG("CREATE_RESULT: format=%s size=%s mips=%u usage=%s class=%s => %s (%d)\n",
        f->name, s->name, s->mips, u->name, class_name, result_name(r), (int)r);
    if (r != VK_SUCCESS) { ++c->create_failures; return; }
    p_vkGetImageMemoryRequirements(device, image, &req);
    type = choose_memory_type(req.memoryTypeBits, mp, required);
    LOG("IMAGE_REQUIREMENTS: format=%s size=%s mips=%u usage=%s class=%s sizeBytes=%llu alignment=%llu typeBits=0x%08X chosenType=%d\n",
        f->name, s->name, s->mips, u->name, class_name, (unsigned long long)req.size,
        (unsigned long long)req.alignment, req.memoryTypeBits, type);
    if (type < 0) {
        if (required & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ++c->no_device_local;
        else ++c->no_host_visible_coherent;
        ERR("MEMTYPE_NEGATIVE: format=%s size=%s usage=%s class=%s has no compatible advertised type (typeBits=0x%08X).\n",
            f->name, s->name, u->name, class_name, req.memoryTypeBits);
        p_vkDestroyImage(device, image, NULL); return;
    }
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)type;
    ++c->allocations;
    g_current_operation = "vkAllocateMemory(BC7 fresh image)";
    r = p_vkAllocateMemory(device, &ai, NULL, &memory);
    g_current_operation = NULL;
    LOG("ALLOC_RESULT: format=%s size=%s usage=%s class=%s type=%d flags=0x%X bytes=%llu => %s (%d)\n",
        f->name, s->name, u->name, class_name, type, mp->memoryTypes[type].propertyFlags,
        (unsigned long long)ai.allocationSize, result_name(r), (int)r);
    if (r != VK_SUCCESS) { ++c->allocation_failures; p_vkDestroyImage(device, image, NULL); return; }
    ++c->binds;
    g_current_operation = "vkBindImageMemory(BC7 fresh image)";
    r = p_vkBindImageMemory(device, image, memory, 0);
    g_current_operation = NULL;
    LOG("BIND_RESULT: format=%s size=%s mips=%u usage=%s class=%s type=%d offset=0 => %s (%d)\n",
        f->name, s->name, s->mips, u->name, class_name, type, result_name(r), (int)r);
    if (r != VK_SUCCESS) ++c->bind_failures;
    else (void)create_required_views(device, image, f->format, s, f->name, u->name, class_name, c);
    p_vkDestroyImage(device, image, NULL); p_vkFreeMemory(device, memory, NULL);
}

static void run_matrix(VkPhysicalDevice physical, VkDevice device, const VkPhysicalDeviceMemoryProperties *mp, MatrixCounts *c) {
    uint32_t fi, ui, si;
    for (fi = 0; fi < ARRAY_LEN(g_formats); ++fi) {
        VkFormatProperties fp;
        p_vkGetPhysicalDeviceFormatProperties(physical, g_formats[fi].format, &fp);
        LOG("FORMAT_PROPERTIES: format=%s(%d) optimalFeatures=0x%X sampled=0x%X transferSrc=0x%X transferDst=0x%X\n",
            g_formats[fi].name, (int)g_formats[fi].format, fp.optimalTilingFeatures,
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
        for (ui = 0; ui < ARRAY_LEN(g_usages); ++ui) {
            VkImageFormatProperties cap; VkResult qr;
            memset(&cap, 0, sizeof(cap)); ++c->queries;
            g_current_operation = "vkGetPhysicalDeviceImageFormatProperties(BC7 matrix)";
            qr = p_vkGetPhysicalDeviceImageFormatProperties(physical, g_formats[fi].format, VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL, g_usages[ui].flags, 0, &cap);
            g_current_operation = NULL;
            LOG("FORMAT_QUERY_RESULT: format=%s usage=%s => %s (%d) maxExtent=%ux%ux%u maxMipLevels=%u maxResourceSize=%llu\n",
                g_formats[fi].name, g_usages[ui].name, result_name(qr), (int)qr, cap.maxExtent.width,
                cap.maxExtent.height, cap.maxExtent.depth, cap.maxMipLevels, (unsigned long long)cap.maxResourceSize);
            if (qr != VK_SUCCESS) { ++c->query_failures; continue; }
            for (si = 0; si < ARRAY_LEN(g_sizes); ++si) {
                const SizeCase *s = &g_sizes[si];
                if (s->width > cap.maxExtent.width || s->height > cap.maxExtent.height || s->mips > cap.maxMipLevels) {
                    ++c->query_failures;
                    ERR("CAPABILITY_FAILURE: successful query limits reject mandatory descriptor format=%s usage=%s size=%s mips=%u.\n",
                        g_formats[fi].name, g_usages[ui].name, s->name, s->mips);
                    continue;
                }
                create_bind_view_one(device, mp, &g_formats[fi], &g_usages[ui], s,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL", c);
                create_bind_view_one(device, mp, &g_formats[fi], &g_usages[ui], s,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    "HOST_VISIBLE|HOST_COHERENT", c);
            }
        }
    }
}

static VkDeviceSize bc7_mip_bytes(uint32_t w, uint32_t h) {
    return (VkDeviceSize)((w + 3u) / 4u) * (VkDeviceSize)((h + 3u) / 4u) * 16u;
}
static void fill_valid_bc7_blocks(uint8_t *dst, VkDeviceSize bytes, uint32_t level) {
    VkDeviceSize off; uint32_t j;
    /* Mode 6 prefix uses least-significant unary bits 0000001.  All remaining
       endpoint/pbit/index bit combinations encode legal mode-6 blocks. */
    for (off = 0; off < bytes; off += 16u) {
        dst[off] = 0x40u;
        for (j = 1; j < 16u; ++j) dst[off + j] = (uint8_t)(0x35u ^ (level * 0x29u) ^ ((uint32_t)(off / 16u) * 0x13u) ^ (j * 0x0Du));
    }
}
static VkResult allocate_bind_buffer(VkDevice device, const VkPhysicalDeviceMemoryProperties *mp, VkDeviceSize size,
    VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory, const char *name) {
    VkBufferCreateInfo bi; VkMemoryRequirements req; VkMemoryAllocateInfo ai; VkResult r; int type;
    memset(&bi, 0, sizeof(bi)); bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = size;
    bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = p_vkCreateBuffer(device, &bi, NULL, buffer);
    LOG("BUFFER_CREATE_RESULT: %s bytes=%llu usage=0x%X => %s (%d)\n", name, (unsigned long long)size, usage, result_name(r), (int)r);
    if (r != VK_SUCCESS) return r;
    p_vkGetBufferMemoryRequirements(device, *buffer, &req);
    type = choose_memory_type(req.memoryTypeBits, mp, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    LOG("BUFFER_REQUIREMENTS: %s reqBytes=%llu alignment=%llu typeBits=0x%08X chosenType=%d\n", name,
        (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits, type);
    if (type < 0) return VK_ERROR_FEATURE_NOT_PRESENT;
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)type;
    r = p_vkAllocateMemory(device, &ai, NULL, memory);
    LOG("BUFFER_ALLOC_RESULT: %s type=%d flags=0x%X bytes=%llu => %s (%d)\n", name, type, mp->memoryTypes[type].propertyFlags,
        (unsigned long long)ai.allocationSize, result_name(r), (int)r);
    if (r != VK_SUCCESS) return r;
    r = p_vkBindBufferMemory(device, *buffer, *memory, 0);
    LOG("BUFFER_BIND_RESULT: %s offset=0 => %s (%d)\n", name, result_name(r), (int)r);
    return r;
}

/* Transfer one BC7 image and compare every 24x24 full-chain mip byte range. */
static int run_upload_case(VkDevice device, VkQueue queue, uint32_t family, const VkPhysicalDeviceMemoryProperties *mp,
                           const FormatCase *f, MatrixCounts *c) {
    const SizeCase s = { "24x24-upload", 24u, 24u, 5u };
    const UsageCase u = { "SAMPLED|TRANSFER_SRC|TRANSFER_DST", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VkImage image = VK_NULL_HANDLE; VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkBuffer up = VK_NULL_HANDLE, down = VK_NULL_HANDLE; VkDeviceMemory up_mem = VK_NULL_HANDLE, down_mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkMemoryRequirements req; VkMemoryAllocateInfo ai; VkImageCreateInfo ii = make_image_info(f->format, u.flags, &s);
    VkBufferImageCopy regions[5]; VkDeviceSize offsets[5], bytes[5], total = 0;
    VkResult r = VK_SUCCESS; int image_type; uint32_t level, x = s.width, y = s.height; uint8_t *mapped = NULL; int bad = 0;
    ++c->upload_cases;
    memset(&req, 0, sizeof(req)); memset(regions, 0, sizeof(regions));
    for (level = 0; level < s.mips; ++level) {
        bytes[level] = bc7_mip_bytes(x, y); offsets[level] = total; total += bytes[level];
        regions[level].bufferOffset = offsets[level]; regions[level].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[level].imageSubresource.mipLevel = level; regions[level].imageSubresource.baseArrayLayer = 0; regions[level].imageSubresource.layerCount = 1;
        regions[level].imageExtent.width = x; regions[level].imageExtent.height = y; regions[level].imageExtent.depth = 1;
        LOG("UPLOAD_LAYOUT: format=%s mip=%u extent=%ux%u blocks=%ux%u bytes=%llu offset=%llu validBC7Mode6Prefix=40\n",
            f->name, level, x, y, (x + 3u) / 4u, (y + 3u) / 4u, (unsigned long long)bytes[level], (unsigned long long)offsets[level]);
        x = x > 1u ? x >> 1u : 1u; y = y > 1u ? y >> 1u : 1u;
    }
    LOG("UPLOAD_CASE_BEGIN: format=%s image=24x24 mips=5 totalCompressedBytes=%llu; exact block transfer/readback, no decode/re-encode comparison.\n",
        f->name, (unsigned long long)total);
    g_current_operation = "vkCreateImage(BC7 upload target)";
    r = p_vkCreateImage(device, &ii, NULL, &image); g_current_operation = NULL;
    LOG("UPLOAD_CREATE_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r);
    if (r != VK_SUCCESS) { bad = 1; goto done; }
    p_vkGetImageMemoryRequirements(device, image, &req); image_type = choose_memory_type(req.memoryTypeBits, mp, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    LOG("UPLOAD_IMAGE_REQUIREMENTS: format=%s bytes=%llu alignment=%llu typeBits=0x%08X chosenDeviceLocal=%d\n", f->name,
        (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits, image_type);
    if (image_type < 0) { ERR("UPLOAD_FAILURE: no compatible DEVICE_LOCAL image memory for %s.\n", f->name); bad = 1; goto done; }
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)image_type;
    r = p_vkAllocateMemory(device, &ai, NULL, &image_mem); LOG("UPLOAD_IMAGE_ALLOC_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r);
    if (r != VK_SUCCESS) { bad = 1; goto done; }
    r = p_vkBindImageMemory(device, image, image_mem, 0); LOG("UPLOAD_IMAGE_BIND_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r);
    if (r != VK_SUCCESS) { bad = 1; goto done; }
    /* Explicit SRV-equivalent check also covers the actual upload target. */
    if (create_required_views(device, image, f->format, &s, f->name, u.name, "DEVICE_LOCAL upload target", c)) bad = 1;
    if (bad) goto done;
    r = allocate_bind_buffer(device, mp, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up, &up_mem, "BC7-upload-src"); if (r != VK_SUCCESS) { bad = 1; goto done; }
    r = allocate_bind_buffer(device, mp, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &down, &down_mem, "BC7-readback-dst"); if (r != VK_SUCCESS) { bad = 1; goto done; }
    r = p_vkMapMemory(device, up_mem, 0, total, 0, (void **)&mapped); LOG("UPLOAD_MAP_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r);
    if (r != VK_SUCCESS) { bad = 1; goto done; }
    for (level = 0; level < s.mips; ++level) fill_valid_bc7_blocks(mapped + offsets[level], bytes[level], level);
    { VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, up_mem, 0, VK_WHOLE_SIZE };
      r = p_vkFlushMappedMemoryRanges(device, 1, &range); LOG("UPLOAD_FLUSH_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r); }
    p_vkUnmapMemory(device, up_mem); mapped = NULL;
    if (r != VK_SUCCESS) { bad = 1; goto done; }
    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, family };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
        VkImageSubresourceRange range; VkImageMemoryBarrier to_dst, to_src; VkBufferMemoryBarrier to_host;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 }; VkSubmitInfo submit;
        memset(&range, 0, sizeof(range)); range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; range.levelCount = s.mips; range.layerCount = 1;
        r = p_vkCreateCommandPool(device, &pi, NULL, &pool); LOG("UPLOAD_CMDPOOL_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r); if (r != VK_SUCCESS) { bad = 1; goto done; }
        cai.commandPool = pool; r = p_vkAllocateCommandBuffers(device, &cai, &cmd); LOG("UPLOAD_CMDBUF_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r); if (r != VK_SUCCESS) { bad = 1; goto done; }
        r = p_vkBeginCommandBuffer(cmd, &begin); LOG("UPLOAD_BEGIN_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r); if (r != VK_SUCCESS) { bad = 1; goto done; }
        memset(&to_dst, 0, sizeof(to_dst)); to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_dst.image = image; to_dst.subresourceRange = range; to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
        g_current_operation = "vkCmdCopyBufferToImage(BC7 all mips)"; p_vkCmdCopyBufferToImage(cmd, up, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, s.mips, regions); g_current_operation = NULL;
        memset(&to_src, 0, sizeof(to_src)); to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_src.image = image; to_src.subresourceRange = range; to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
        g_current_operation = "vkCmdCopyImageToBuffer(BC7 all mips)"; p_vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down, s.mips, regions); g_current_operation = NULL;
        memset(&to_host, 0, sizeof(to_host)); to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.buffer = down; to_host.size = total; to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &to_host, 0, NULL);
        r = p_vkEndCommandBuffer(cmd); LOG("UPLOAD_END_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r); if (r != VK_SUCCESS) { bad = 1; goto done; }
        r = p_vkCreateFence(device, &fi, NULL, &fence); LOG("UPLOAD_FENCE_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r); if (r != VK_SUCCESS) { bad = 1; goto done; }
        memset(&submit, 0, sizeof(submit)); submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        g_current_operation = "vkQueueSubmit(BC7 exact transfer)"; r = p_vkQueueSubmit(queue, 1, &submit, fence); g_current_operation = NULL;
        LOG("UPLOAD_SUBMIT_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r); if (r != VK_SUCCESS) { bad = 1; goto done; }
        g_current_operation = "vkWaitForFences(BC7 exact transfer 15s)"; r = p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(15000000000)); g_current_operation = NULL;
        LOG("UPLOAD_WAIT_RESULT: format=%s timeoutNs=15000000000 => %s (%d)\n", f->name, result_name(r), (int)r); if (r != VK_SUCCESS) { bad = 1; goto done; }
    }
    r = p_vkMapMemory(device, down_mem, 0, total, 0, (void **)&mapped); LOG("READBACK_MAP_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r);
    if (r != VK_SUCCESS) { bad = 1; goto done; }
    { VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, down_mem, 0, VK_WHOLE_SIZE };
      r = p_vkInvalidateMappedMemoryRanges(device, 1, &range); LOG("READBACK_INVALIDATE_RESULT: format=%s => %s (%d)\n", f->name, result_name(r), (int)r); }
    if (r != VK_SUCCESS) { bad = 1; goto done; }
    x = s.width; y = s.height;
    for (level = 0; level < s.mips; ++level) {
        uint8_t *expect = (uint8_t *)malloc((size_t)bytes[level]);
        int match = 0;
        if (expect) {
            fill_valid_bc7_blocks(expect, bytes[level], level);
            match = memcmp(mapped + offsets[level], expect, (size_t)bytes[level]) == 0;
            if (!match) {
                VkDeviceSize k, first = 0, limit = bytes[level] < 32u ? bytes[level] : 32u;
                for (k = 0; k < bytes[level]; ++k) {
                    if (mapped[offsets[level] + k] != expect[k]) { first = k; break; }
                }
                LOG("READBACK_MISMATCH_DETAIL: format=%s mip=%u firstByte=%llu expected=%02X actual=%02X\n",
                    f->name, level, (unsigned long long)first, expect[first], mapped[offsets[level] + first]);
                LOG("EXPECTED_FIRST_BYTES:"); for (k = 0; k < limit; ++k) LOG(" %02X", expect[k]); LOG("\n");
                LOG("ACTUAL_FIRST_BYTES:"); for (k = 0; k < limit; ++k) LOG(" %02X", mapped[offsets[level] + k]); LOG("\n");
            }
            free(expect);
        }
        LOG("READBACK_MIP_RESULT: format=%s mip=%u extent=%ux%u bytes=%llu offset=%llu => %s\n", f->name, level, x, y,
            (unsigned long long)bytes[level], (unsigned long long)offsets[level], match ? "EXACT_MATCH" : "MISMATCH");
        if (!match) { ++c->readback_mip_mismatches; bad = 1; }
        x = x > 1u ? x >> 1u : 1u; y = y > 1u ? y >> 1u : 1u;
    }

done:
    if (mapped && down_mem) p_vkUnmapMemory(device, down_mem);
    else if (mapped && up_mem) p_vkUnmapMemory(device, up_mem);
    if (fence) p_vkDestroyFence(device, fence, NULL);
    if (pool) p_vkDestroyCommandPool(device, pool, NULL);
    if (down) p_vkDestroyBuffer(device, down, NULL); if (up) p_vkDestroyBuffer(device, up, NULL);
    if (image) p_vkDestroyImage(device, image, NULL);
    if (down_mem) p_vkFreeMemory(device, down_mem, NULL); if (up_mem) p_vkFreeMemory(device, up_mem, NULL); if (image_mem) p_vkFreeMemory(device, image_mem, NULL);
    if (bad) { ++c->upload_failures; ERR("UPLOAD_CASE_FINAL: format=%s => FAIL (real operation result preserved above).\n", f->name); }
    else LOG("UPLOAD_CASE_FINAL: format=%s => PASS all 5 mip byte ranges exact.\n", f->name);
    return bad;
}

static void usage(const wchar_t *exe) {
    LOG("Usage:\n  %ls --loader C:\\absolute\\vulkan-1.dll --icd C:\\absolute\\ashesD3D11Renderer_icd.json [--allow-non-intel-test]\n", exe);
    LOG("The raw BC7 matrix always runs both formats, both usages, all six requested sizes, per-class fresh binds, SRV views, and exact 24x24 transfer/readback.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL; int allow_non_intel = 0, i, status = 1;
    HMODULE loader = NULL; union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } l;
    VkInstance instance = VK_NULL_HANDLE; VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE; int debug_enabled = 0;
    VkPhysicalDevice *list = NULL, physical = VK_NULL_HANDLE; uint32_t physical_count = 0, family = UINT32_MAX;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mp; VkDevice device = VK_NULL_HANDLE; VkQueue queue = VK_NULL_HANDLE;
    MatrixCounts c; const char *first_failure = NULL;
    memset(&props, 0, sizeof(props)); memset(&mp, 0, sizeof(mp)); memset(&c, 0, sizeof(c));
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
    LOG("RimWorld BC7 raw Vulkan resource matrix\nLOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    LOG("POLICY: this process makes no no-BC fallback decision. It requests only real BC7 resources.\n");
    if (allow_non_intel) LOG("WARNING: --allow-non-intel-test is Wine sandbox integration only, NOT HD 5500 validation.\n");
    loader = LoadLibraryW(loader_path); if (!loader) { status = 4; first_failure = "LoadLibraryW(loader)"; goto cleanup; }
    l.raw = GetProcAddress(loader, "vkGetInstanceProcAddr"); gipa = l.typed; if (!gipa) { status = 4; first_failure = "vkGetInstanceProcAddr"; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion); LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties); LOAD_GLOBAL(vkCreateInstance);
    if (!p_vkEnumerateInstanceExtensionProperties || !p_vkCreateInstance) { status = 20; first_failure = "global entrypoint"; goto cleanup; }
    if (p_vkEnumerateInstanceVersion) { uint32_t v = VK_API_VERSION_1_0; VkResult r = p_vkEnumerateInstanceVersion(&v); LOG("RESULT: vkEnumerateInstanceVersion => %s (%d), max=%u.%u.%u\n", result_name(r), (int)r, VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v)); if (r != VK_SUCCESS) { status = 21; first_failure = "vkEnumerateInstanceVersion"; goto cleanup; } }
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "RimWorld BC7 raw matrix", 1u, "standalone", 1u, VK_API_VERSION_1_0 };
        VkInstanceCreateInfo ci; VkDebugUtilsMessengerCreateInfoEXT di; const char *ext[2]; uint32_t n = 0; VkResult r;
        memset(&ci, 0, sizeof(ci)); memset(&di, 0, sizeof(di)); ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo = &app;
        if (instance_extension_present(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) { ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; ext[n++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME; }
        if (instance_extension_present(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) { debug_enabled = 1; ext[n++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME; di.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT; di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT; di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT; di.pfnUserCallback = debug_callback; ci.pNext = &di; }
        ci.enabledExtensionCount = n; ci.ppEnabledExtensionNames = ext; r = p_vkCreateInstance(&ci, NULL, &instance); LOG("RESULT: vkCreateInstance(api=1.0) => %s (%d)\n", result_name(r), (int)r); if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateInstance"; goto cleanup; }
    }
    LOAD_INSTANCE(instance, vkDestroyInstance); LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices); LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceImageFormatProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceFormatProperties); LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties); LOAD_INSTANCE(instance, vkCreateDevice); LOAD_INSTANCE(instance, vkGetDeviceProcAddr); LOAD_INSTANCE(instance, vkCreateDebugUtilsMessengerEXT); LOAD_INSTANCE(instance, vkDestroyDebugUtilsMessengerEXT);
    if (!p_vkDestroyInstance || !p_vkEnumeratePhysicalDevices || !p_vkGetPhysicalDeviceProperties || !p_vkGetPhysicalDeviceMemoryProperties || !p_vkGetPhysicalDeviceQueueFamilyProperties || !p_vkGetPhysicalDeviceImageFormatProperties || !p_vkGetPhysicalDeviceFormatProperties || !p_vkEnumerateDeviceExtensionProperties || !p_vkCreateDevice || !p_vkGetDeviceProcAddr) { status = 20; first_failure = "instance entrypoint"; goto cleanup; }
    if (debug_enabled && p_vkCreateDebugUtilsMessengerEXT && p_vkDestroyDebugUtilsMessengerEXT) { VkDebugUtilsMessengerCreateInfoEXT di; VkResult r; memset(&di, 0, sizeof(di)); di.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT; di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT; di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT; di.pfnUserCallback = debug_callback; r = p_vkCreateDebugUtilsMessengerEXT(instance, &di, NULL, &messenger); LOG("RESULT: vkCreateDebugUtilsMessengerEXT => %s (%d)\n", result_name(r), (int)r); }
    { VkResult r = p_vkEnumeratePhysicalDevices(instance, &physical_count, NULL); LOG("RESULT: vkEnumeratePhysicalDevices(count) => %s (%d), count=%u\n", result_name(r), (int)r, physical_count); if (r != VK_SUCCESS || !physical_count) { status = 5; first_failure = "physical device enumeration"; goto cleanup; } }
    if (!(list = (VkPhysicalDevice *)calloc(physical_count, sizeof(*list)))) { status = 5; first_failure = "physical device list allocation"; goto cleanup; }
    { VkResult r = p_vkEnumeratePhysicalDevices(instance, &physical_count, list); if (r != VK_SUCCESS) { status = 5; first_failure = "physical device enumeration list"; goto cleanup; } }
    for (i = 0; i < (int)physical_count; ++i) { VkPhysicalDeviceProperties q; uint32_t candidate; p_vkGetPhysicalDeviceProperties(list[i], &q); LOG("ADAPTER[%d]: name=%s vendor=0x%04X device=0x%04X type=%s%s\n", i, q.deviceName, q.vendorID, q.deviceID, device_type_name(q.deviceType), looks_software(&q) ? " [software-like]" : ""); if (!allow_non_intel && (q.vendorID != INTEL_VENDOR_ID || looks_software(&q))) continue; if (!physical && find_transfer_queue(list[i], &candidate)) { physical = list[i]; family = candidate; props = q; } }
    if (!physical) { status = 6; first_failure = "eligible adapter selection"; goto cleanup; }
    LOG("SELECTED: %s vendor=0x%04X queueFamily=%u\n", props.deviceName, props.vendorID, family); if (props.vendorID != INTEL_VENDOR_ID || looks_software(&props)) LOG("WARNING: selected adapter is synthetic/non-Intel; do not treat this as HD 5500 compatibility evidence.\n");
    p_vkGetPhysicalDeviceMemoryProperties(physical, &mp); for (i = 0; i < (int)mp.memoryTypeCount; ++i) LOG("MEMORY_TYPE[%d]: heap=%u flags=0x%X\n", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags);
    { float priority = 1.0f; VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0u, family, 1u, &priority }; VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0u, 1u, &qi, 0u, NULL, 0u, NULL, NULL }; const char *subset = "VK_KHR_portability_subset"; VkResult r; if (device_extension_present(physical, subset)) { ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = &subset; } r = p_vkCreateDevice(physical, &ci, NULL, &device); LOG("RESULT: vkCreateDevice => %s (%d)\n", result_name(r), (int)r); if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateDevice"; goto cleanup; } }
    LOAD_DEVICE(device, vkDestroyDevice); LOAD_DEVICE(device, vkGetDeviceQueue); LOAD_DEVICE(device, vkCreateImage); LOAD_DEVICE(device, vkDestroyImage); LOAD_DEVICE(device, vkGetImageMemoryRequirements); LOAD_DEVICE(device, vkAllocateMemory); LOAD_DEVICE(device, vkFreeMemory); LOAD_DEVICE(device, vkBindImageMemory); LOAD_DEVICE(device, vkCreateImageView); LOAD_DEVICE(device, vkDestroyImageView); LOAD_DEVICE(device, vkCreateBuffer); LOAD_DEVICE(device, vkDestroyBuffer); LOAD_DEVICE(device, vkGetBufferMemoryRequirements); LOAD_DEVICE(device, vkBindBufferMemory); LOAD_DEVICE(device, vkMapMemory); LOAD_DEVICE(device, vkUnmapMemory); LOAD_DEVICE(device, vkFlushMappedMemoryRanges); LOAD_DEVICE(device, vkInvalidateMappedMemoryRanges); LOAD_DEVICE(device, vkCreateCommandPool); LOAD_DEVICE(device, vkDestroyCommandPool); LOAD_DEVICE(device, vkAllocateCommandBuffers); LOAD_DEVICE(device, vkBeginCommandBuffer); LOAD_DEVICE(device, vkEndCommandBuffer); LOAD_DEVICE(device, vkCmdPipelineBarrier); LOAD_DEVICE(device, vkCmdCopyBufferToImage); LOAD_DEVICE(device, vkCmdCopyImageToBuffer); LOAD_DEVICE(device, vkCreateFence); LOAD_DEVICE(device, vkDestroyFence); LOAD_DEVICE(device, vkQueueSubmit); LOAD_DEVICE(device, vkWaitForFences); LOAD_DEVICE(device, vkDeviceWaitIdle);
    if (!p_vkDestroyDevice || !p_vkGetDeviceQueue || !p_vkCreateImage || !p_vkDestroyImage || !p_vkGetImageMemoryRequirements || !p_vkAllocateMemory || !p_vkFreeMemory || !p_vkBindImageMemory || !p_vkCreateImageView || !p_vkDestroyImageView || !p_vkCreateBuffer || !p_vkDestroyBuffer || !p_vkGetBufferMemoryRequirements || !p_vkBindBufferMemory || !p_vkMapMemory || !p_vkUnmapMemory || !p_vkFlushMappedMemoryRanges || !p_vkInvalidateMappedMemoryRanges || !p_vkCreateCommandPool || !p_vkDestroyCommandPool || !p_vkAllocateCommandBuffers || !p_vkBeginCommandBuffer || !p_vkEndCommandBuffer || !p_vkCmdPipelineBarrier || !p_vkCmdCopyBufferToImage || !p_vkCmdCopyImageToBuffer || !p_vkCreateFence || !p_vkDestroyFence || !p_vkQueueSubmit || !p_vkWaitForFences || !p_vkDeviceWaitIdle) { status = 20; first_failure = "device entrypoint"; goto cleanup; }
    p_vkGetDeviceQueue(device, family, 0, &queue); if (!queue) { status = 7; first_failure = "vkGetDeviceQueue"; goto cleanup; }
    run_matrix(physical, device, &mp, &c);
    for (i = 0; i < (int)ARRAY_LEN(g_formats); ++i) (void)run_upload_case(device, queue, family, &mp, &g_formats[i], &c);
    { VkResult r = p_vkDeviceWaitIdle(device); LOG("RESULT: vkDeviceWaitIdle => %s (%d)\n", result_name(r), (int)r); if (r != VK_SUCCESS) { status = 21; first_failure = "vkDeviceWaitIdle"; } }
    if (status == 1) {
        if (g_debug_errors) { status = 10; first_failure = "debug-utils error callback"; }
        else if (c.query_failures || c.create_failures || c.allocation_failures || c.bind_failures || c.full_view_failures || c.tail_view_failures || c.no_device_local || c.no_host_visible_coherent || c.upload_failures || c.readback_mip_mismatches) { status = 11; first_failure = "BC7 matrix operation failure"; }
        else status = 0;
    }
cleanup:
    LOG("BC7_MATRIX_SUMMARY: queries=%u queryFailures=%u creates=%u createFailures=%u allocations=%u allocationFailures=%u binds=%u bindFailures=%u fullViews=%u fullViewFailures=%u tailViews=%u tailViewFailures=%u noDeviceLocal=%u noHostVisibleCoherent=%u uploadCases=%u uploadFailures=%u readbackMipMismatches=%u\n", c.queries, c.query_failures, c.creates, c.create_failures, c.allocations, c.allocation_failures, c.binds, c.bind_failures, c.full_views, c.full_view_failures, c.tail_views, c.tail_view_failures, c.no_device_local, c.no_host_visible_coherent, c.upload_cases, c.upload_failures, c.readback_mip_mismatches);
    if (device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(device); if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL); free(list); if (messenger && p_vkDestroyDebugUtilsMessengerEXT) p_vkDestroyDebugUtilsMessengerEXT(instance, messenger, NULL); if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL); if (loader) FreeLibrary(loader);
    LOG("DEBUG_SUMMARY: total=%ld errors=%ld textIntegrity=%s\n", g_debug_total, g_debug_errors, g_debug_bad_text ? "SUSPECT" : "sane-or-no-messages"); if (g_first_debug_operation) LOG("FIRST_DEBUG_ERROR_OPERATION: %s\n", g_first_debug_operation);
    if (status) ERR("FINAL: FAIL status=%d; FIRST_FAILURE: %s\n", status, first_failure ? first_failure : "unspecified"); else LOG("FINAL: PASS status=0; BC7 resources, views, and exact tail-mip transfer/readback all succeeded.\n");
    return status;
}
