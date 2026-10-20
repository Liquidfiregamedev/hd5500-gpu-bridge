/*
 * CapabilityConsistencyProbe: integration probe for the v12 Ashes D3D11 ICD.
 * It deliberately uses only Vulkan 1.0 entry points through a caller-selected
 * loader and ICD, then compares advertised format capabilities with image
 * creation/allocation/binding.  It is not a real Intel HD 5500 validation.
 */
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <ctype.h>
#include <stdarg.h>
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
#define MAX_IMAGE_BYTES (UINT64_C(64) * 1024u * 1024u)

typedef struct Counts {
    uint32_t queries;
    uint32_t query_successes;
    uint32_t creations;
    uint32_t create_successes;
    uint32_t allocations;
    uint32_t binds;
    uint32_t oom_accepted;
    uint32_t mismatches;
    uint32_t info_mismatches;
} Counts;

typedef struct UsageCase {
    const char *name;
    VkImageUsageFlags usage;
} UsageCase;

static const UsageCase g_usages[] = {
    { "SAMPLED|TRANSFER_SRC|TRANSFER_DST", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT },
    { "TRANSFER_SRC|TRANSFER_DST", VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT },
    { "SAMPLED|STORAGE", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT },
    { "SAMPLED|COLOR_ATTACHMENT", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT },
    { "DEPTH_STENCIL_ATTACHMENT|SAMPLED", VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT }
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
static PFN_vkGetPhysicalDeviceFormatProperties p_vkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceImageFormatProperties p_vkGetPhysicalDeviceImageFormatProperties;
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkDestroyDevice p_vkDestroyDevice;
static PFN_vkCreateImage p_vkCreateImage;
static PFN_vkDestroyImage p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindImageMemory p_vkBindImageMemory;

#define LOAD_GLOBAL(n) p_##n = (PFN_##n)gipa(VK_NULL_HANDLE, #n)
#define LOAD_INSTANCE(i, n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d, n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)

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

static int is_oom(VkResult r) {
    return r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY;
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
    uint32_t n = 0u, i;
    VkQueueFamilyProperties *p;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, NULL);
    if (!n || !(p = (VkQueueFamilyProperties *)calloc(n, sizeof(*p)))) return 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, p);
    for (i = 0u; i < n; ++i) {
        LOG("QUEUE[%u]: flags=0x%X count=%u\n", i, p[i].queueFlags, p[i].queueCount);
        if (p[i].queueCount && (p[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
            *out = i;
            free(p);
            return 1;
        }
    }
    free(p);
    return 0;
}

static void mismatch(Counts *c, int informational, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (informational) {
        fputs("INFO-MISMATCH: ", stderr);
        vfprintf(stderr, fmt, ap);
        ++c->info_mismatches;
    } else {
        fputs("MISMATCH: ", stderr);
        vfprintf(stderr, fmt, ap);
        ++c->mismatches;
    }
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static int is_decodable_bc(VkFormat f) {
    switch (f) {
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
        return 1;
    default:
        return 0;
    }
}

static int is_bc7(VkFormat f) {
    return f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK;
}

static VkFormatFeatureFlags tiling_features(const VkFormatProperties *p, VkImageTiling tiling) {
    return tiling == VK_IMAGE_TILING_OPTIMAL ? p->optimalTilingFeatures : p->linearTilingFeatures;
}

static int features_allow_usage(VkFormatFeatureFlags f, VkImageUsageFlags usage) {
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return 0;
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return 0;
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return 0;
    if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && !(f & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) return 0;
    if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && !(f & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) return 0;
    return 1;
}

static int choose_memory_type(uint32_t bits, const VkPhysicalDeviceMemoryProperties *mp) {
    uint32_t i;
    for (i = 0u; i < mp->memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return (int)i;
    for (i = 0u; i < mp->memoryTypeCount; ++i)
        if (bits & (1u << i)) return (int)i;
    return -1;
}

static uint32_t full_mips(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t largest = w;
    uint32_t levels = 1u;
    if (h > largest) largest = h;
    if (d > largest) largest = d;
    while (largest > 1u) {
        largest >>= 1u;
        ++levels;
    }
    return levels;
}

static VkExtent3D small_extent(VkImageType type, int odd) {
    VkExtent3D e;
    e.width = odd ? 13u : 1u;
    e.height = type == VK_IMAGE_TYPE_1D ? 1u : (odd ? 7u : 1u);
    e.depth = type == VK_IMAGE_TYPE_3D ? (odd ? 5u : 1u) : 1u;
    return e;
}

static int extent_within(VkExtent3D e, const VkImageFormatProperties *p) {
    return e.width <= p->maxExtent.width && e.height <= p->maxExtent.height && e.depth <= p->maxExtent.depth;
}

static void create_bind_image(VkDevice device, const VkPhysicalDeviceMemoryProperties *mp,
    VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
    VkExtent3D extent, uint32_t mips, const char *label, Counts *c, int informational) {
    VkImageCreateInfo ii;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkResult r;
    int mem_type;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = type;
    ii.format = format;
    ii.extent = extent;
    ii.mipLevels = mips;
    ii.arrayLayers = 1u;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = tiling;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ++c->creations;
    r = p_vkCreateImage(device, &ii, NULL, &image);
    LOG("CREATE_RESULT: case=%s format=%d type=%u tiling=%u usage=0x%X extent=%ux%ux%u mips=%u => %s (%d)\n",
        label, (int)format, (unsigned)type, (unsigned)tiling, (unsigned)usage, extent.width, extent.height,
        extent.depth, mips, result_name(r), (int)r);
    if (r != VK_SUCCESS) {
        if (is_oom(r)) {
            ++c->oom_accepted;
            LOG("OOM_ACCEPTED: stage=create case=%s\n", label);
        } else {
            mismatch(c, informational, "query-success create failure case=%s format=%d usage=0x%X result=%s (%d)",
                label, (int)format, (unsigned)usage, result_name(r), (int)r);
        }
        return;
    }
    ++c->create_successes;
    memset(&req, 0, sizeof(req));
    p_vkGetImageMemoryRequirements(device, image, &req);
    LOG("IMAGE_REQUIREMENTS: case=%s bytes=%llu alignment=%llu typeBits=0x%08X\n", label,
        (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits);
    if (req.size > MAX_IMAGE_BYTES) {
        mismatch(c, informational, "memory guard exceeded for small case=%s bytes=%llu (>64MiB)",
            label, (unsigned long long)req.size);
        p_vkDestroyImage(device, image, NULL);
        return;
    }
    mem_type = choose_memory_type(req.memoryTypeBits, mp);
    if (mem_type < 0) {
        mismatch(c, informational, "query-success image has no compatible advertised memory type case=%s typeBits=0x%08X",
            label, req.memoryTypeBits);
        p_vkDestroyImage(device, image, NULL);
        return;
    }
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)mem_type;
    ++c->allocations;
    r = p_vkAllocateMemory(device, &ai, NULL, &memory);
    LOG("ALLOC_RESULT: case=%s type=%d flags=0x%X bytes=%llu => %s (%d)\n", label, mem_type,
        (unsigned)mp->memoryTypes[mem_type].propertyFlags, (unsigned long long)ai.allocationSize,
        result_name(r), (int)r);
    if (r != VK_SUCCESS) {
        if (is_oom(r)) {
            ++c->oom_accepted;
            LOG("OOM_ACCEPTED: stage=allocate case=%s\n", label);
        } else {
            mismatch(c, informational, "query-success allocation failure case=%s format=%d result=%s (%d)",
                label, (int)format, result_name(r), (int)r);
        }
        p_vkDestroyImage(device, image, NULL);
        return;
    }
    ++c->binds;
    r = p_vkBindImageMemory(device, image, memory, 0u);
    LOG("BIND_RESULT: case=%s type=%d => %s (%d)\n", label, mem_type, result_name(r), (int)r);
    if (r != VK_SUCCESS) {
        if (is_oom(r)) {
            ++c->oom_accepted;
            LOG("OOM_ACCEPTED: stage=bind case=%s\n", label);
        } else {
            mismatch(c, informational, "query-success bind failure case=%s format=%d result=%s (%d)",
                label, (int)format, result_name(r), (int)r);
        }
    }
    p_vkDestroyImage(device, image, NULL);
    p_vkFreeMemory(device, memory, NULL);
}

static VkResult query_image(VkPhysicalDevice physical, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage, VkImageFormatProperties *props, Counts *c) {
    VkResult r;
    memset(props, 0, sizeof(*props));
    ++c->queries;
    r = p_vkGetPhysicalDeviceImageFormatProperties(physical, format, type, tiling, usage, 0u, props);
    if (r == VK_SUCCESS) ++c->query_successes;
    LOG("FORMAT_QUERY: format=%d type=%u tiling=%u usage=0x%X => %s (%d) max=%ux%ux%u mips=%u samples=0x%X resource=%llu\n",
        (int)format, (unsigned)type, (unsigned)tiling, (unsigned)usage, result_name(r), (int)r,
        props->maxExtent.width, props->maxExtent.height, props->maxExtent.depth, props->maxMipLevels,
        (unsigned)props->sampleCounts, (unsigned long long)props->maxResourceSize);
    return r;
}

static void run_heap_checks(VkPhysicalDevice physical, const VkPhysicalDeviceMemoryProperties *mp,
    Counts *c, int informational) {
    uint32_t i;
    uint32_t before = c->mismatches + c->info_mismatches;
    (void)physical;
    LOG("MEMORY_PROPERTIES: heaps=%u types=%u\n", mp->memoryHeapCount, mp->memoryTypeCount);
    if (!mp->memoryHeapCount || !mp->memoryTypeCount) mismatch(c, informational, "empty memory heap/type table heaps=%u types=%u", mp->memoryHeapCount, mp->memoryTypeCount);
    for (i = 0u; i < mp->memoryHeapCount; ++i) {
        LOG("MEMORY_HEAP[%u]: size=%llu (0x%llX) flags=0x%X\n", i, (unsigned long long)mp->memoryHeaps[i].size,
            (unsigned long long)mp->memoryHeaps[i].size, (unsigned)mp->memoryHeaps[i].flags);
        if (mp->memoryHeaps[i].size == 0u || mp->memoryHeaps[i].size >= UINT64_C(0xFFFFFFFF00000000))
            mismatch(c, informational, "untruthful heap size heap=%u size=%llu", i, (unsigned long long)mp->memoryHeaps[i].size);
    }
    for (i = 0u; i < mp->memoryTypeCount; ++i) {
        LOG("MEMORY_TYPE[%u]: heap=%u flags=0x%X\n", i, mp->memoryTypes[i].heapIndex, (unsigned)mp->memoryTypes[i].propertyFlags);
        if (mp->memoryTypes[i].heapIndex >= mp->memoryHeapCount)
            mismatch(c, informational, "memory type %u references absent heap %u", i, mp->memoryTypes[i].heapIndex);
    }
    if (c->mismatches + c->info_mismatches == before) LOG("CASE PASS: memory-heaps-truthful\n");
    else if (informational) LOG("CASE PASS: memory-heaps-truthful informational\n");
    else LOG("CASE FAIL: memory-heaps-truthful\n");
}

static void run_bc_checks(VkPhysicalDevice physical, VkDevice device, const VkPhysicalDeviceMemoryProperties *mp,
    Counts *c, int informational, int expect_no_decode) {
    VkFormat f;
    uint32_t before_features = c->mismatches + c->info_mismatches;
    uint32_t before_limits;
    uint32_t before_create;
    const VkFormatFeatureFlags forbidden = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT;
    for (f = VK_FORMAT_BC1_RGB_UNORM_BLOCK; f <= VK_FORMAT_BC7_SRGB_BLOCK; f = (VkFormat)(f + 1)) {
        VkFormatProperties fp;
        VkImageFormatProperties cap;
        VkResult r;
        VkFormatFeatureFlags opt;
        VkFormatFeatureFlags lin;
        p_vkGetPhysicalDeviceFormatProperties(physical, f, &fp);
        opt = fp.optimalTilingFeatures;
        lin = fp.linearTilingFeatures;
        LOG("BC_FORMAT_PROPERTIES: format=%d optimal=0x%X linear=0x%X buffer=0x%X\n", (int)f,
            (unsigned)opt, (unsigned)lin, (unsigned)fp.bufferFeatures);
        if ((opt | lin) & forbidden)
            mismatch(c, informational, "BC forbidden feature format=%d optimal=0x%X linear=0x%X forbidden=0x%X",
                (int)f, (unsigned)opt, (unsigned)lin, (unsigned)forbidden);
        if (fp.bufferFeatures != 0u)
            mismatch(c, informational, "BC bufferFeatures must be zero format=%d actual=0x%X", (int)f, (unsigned)fp.bufferFeatures);
        if (!is_bc7(f) && ((opt | lin) & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            mismatch(c, informational, "BC BLIT_SRC advertised outside BC7 format=%d features=0x%X", (int)f, (unsigned)(opt | lin));
        if (is_bc7(f) && !(opt & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            mismatch(c, informational, "BC7 BLIT_SRC not advertised format=%d optimal=0x%X", (int)f, (unsigned)opt);
        r = query_image(physical, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &cap, c);
        {
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if ((opt & need) != need) {
                /* The adapter does not offer this BC format (e.g. CheckFormatSupport lacks it):
                   a refusal is the consistent answer; a success would be an over-claim. */
                if (r == VK_SUCCESS)
                    mismatch(c, informational, "BC query succeeded without advertised SAMPLED|TRANSFER features format=%d optimal=0x%X", (int)f, (unsigned)opt);
                else
                    LOG("BC_FORMAT_UNAVAILABLE_CONSISTENT: format=%d optimal=0x%X query=%s\n", (int)f, (unsigned)opt, result_name(r));
                goto bc_after_sampled_query;
            }
        }
        if (r != VK_SUCCESS) {
            mismatch(c, informational, "BC sampled/transfer query unexpectedly failed format=%d result=%s", (int)f, result_name(r));
        } else {
            uint32_t expected_limit = (!expect_no_decode && is_decodable_bc(f)) ? 16384u : 4096u;
            if (cap.sampleCounts != VK_SAMPLE_COUNT_1_BIT)
                mismatch(c, informational, "BC sampleCounts format=%d actual=0x%X expected=VK_SAMPLE_COUNT_1_BIT", (int)f, (unsigned)cap.sampleCounts);
            if ((!expect_no_decode && is_decodable_bc(f) &&
                    (cap.maxExtent.width < 16384u || cap.maxExtent.height < 16384u)) ||
                ((expect_no_decode || !is_decodable_bc(f)) &&
                    (cap.maxExtent.width != expected_limit || cap.maxExtent.height != expected_limit))) {
                mismatch(c, informational, "BC extent limit format=%d decodable=%d fallbackOff=%d actual=%ux%u expected=%u",
                    (int)f, is_decodable_bc(f), expect_no_decode, cap.maxExtent.width, cap.maxExtent.height, expected_limit);
            }
        }
bc_after_sampled_query:
        r = query_image(physical, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT, &cap, c);
        if (r != VK_ERROR_FORMAT_NOT_SUPPORTED)
            mismatch(c, informational, "BC storage query must be VK_ERROR_FORMAT_NOT_SUPPORTED format=%d got=%s", (int)f, result_name(r));
        r = query_image(physical, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &cap, c);
        if (r != VK_ERROR_FORMAT_NOT_SUPPORTED)
            mismatch(c, informational, "BC color-attachment query must be VK_ERROR_FORMAT_NOT_SUPPORTED format=%d got=%s", (int)f, result_name(r));
    }
    if (c->mismatches + c->info_mismatches == before_features) LOG("CASE PASS: bc-feature-rules\n");
    else if (informational) LOG("CASE PASS: bc-feature-rules informational\n");
    else LOG("CASE FAIL: bc-feature-rules\n");

    before_limits = c->mismatches + c->info_mismatches;
    {
        const VkExtent3D special[] = { { 5333u, 12u, 1u }, { 4097u, 4u, 1u } };
        uint32_t i;
        for (i = 0u; i < ARRAY_LEN(special); ++i) {
            VkImageFormatProperties cap;
            VkResult r = query_image(physical, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &cap, c);
            int within = r == VK_SUCCESS && special[i].width <= cap.maxExtent.width && special[i].height <= cap.maxExtent.height;
            LOG("BC7_CONCRETE_QUERY: extent=%ux%u fallbackOff=%d withinReportedLimits=%d\n",
                special[i].width, special[i].height, expect_no_decode, within);
            if (expect_no_decode) {
                if (within) mismatch(c, informational, "fallback-off BC7 extent %ux%u incorrectly within reported %ux%u",
                    special[i].width, special[i].height, cap.maxExtent.width, cap.maxExtent.height);
            } else if (!within) {
                mismatch(c, informational, "fallback-on BC7 extent %ux%u not within reported %ux%u",
                    special[i].width, special[i].height, cap.maxExtent.width, cap.maxExtent.height);
            }
        }
    }
    if (c->mismatches + c->info_mismatches == before_limits) LOG("CASE PASS: bc-decode-fallback-query-limits\n");
    else if (informational) LOG("CASE PASS: bc-decode-fallback-query-limits informational\n");
    else LOG("CASE FAIL: bc-decode-fallback-query-limits\n");

    before_create = c->mismatches + c->info_mismatches;
    if (!expect_no_decode) {
        create_bind_image(device, mp, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            (VkExtent3D){ 5333u, 12u, 1u }, 1u, "bc7-decoded-fallback-5333x12", c, informational);
        create_bind_image(device, mp, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            (VkExtent3D){ 4097u, 4u, 1u }, 1u, "bc7-decoded-fallback-4097x4", c, informational);
    } else {
        LOG("BC7_CONCRETE_CREATE: SKIPPED fallback-off; the requested extents are outside reported 4096 limits and creation would be invalid usage.\n");
    }
    if (c->mismatches + c->info_mismatches == before_create) LOG("CASE PASS: bc-concrete-creation\n");
    else if (informational) LOG("CASE PASS: bc-concrete-creation informational\n");
    else LOG("CASE FAIL: bc-concrete-creation\n");
}

static void one_generic_query(VkPhysicalDevice physical, VkDevice device, const VkPhysicalDeviceMemoryProperties *mp,
    VkFormat format, VkImageType type, VkImageTiling tiling, const UsageCase *u, Counts *c, int informational) {
    VkFormatProperties fp;
    VkImageFormatProperties cap;
    VkResult r;
    VkFormatFeatureFlags features;
    int advertised;
    VkExtent3D one;
    VkExtent3D odd;
    uint32_t mips;
    char label[192];
    p_vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
    features = tiling_features(&fp, tiling);
    advertised = features_allow_usage(features, u->usage);
    r = query_image(physical, format, type, tiling, u->usage, &cap, c);
    if ((r == VK_SUCCESS) != advertised) {
        mismatch(c, informational, "feature/query disagreement format=%d type=%u tiling=%u usage=%s features=0x%X query=%s",
            (int)format, (unsigned)type, (unsigned)tiling, u->name, (unsigned)features, result_name(r));
    } else if (r != VK_SUCCESS && r != VK_ERROR_FORMAT_NOT_SUPPORTED) {
        mismatch(c, informational, "non-standard format-query failure format=%d type=%u tiling=%u usage=%s result=%s (%d)",
            (int)format, (unsigned)type, (unsigned)tiling, u->name, result_name(r), (int)r);
    }
    if (r != VK_SUCCESS) return;
    if (!cap.maxMipLevels || !cap.maxExtent.width || !cap.maxExtent.height || !cap.maxExtent.depth) {
        mismatch(c, informational, "successful query has zero limit format=%d type=%u", (int)format, (unsigned)type);
        return;
    }
    one = small_extent(type, 0);
    odd = small_extent(type, 1);
    if (!extent_within(one, &cap)) {
        mismatch(c, informational, "successful query excludes mandatory 1x1 descriptor format=%d type=%u max=%ux%ux%u",
            (int)format, (unsigned)type, cap.maxExtent.width, cap.maxExtent.height, cap.maxExtent.depth);
        return;
    }
    snprintf(label, sizeof(label), "generic-1x1-f%d-t%u-i%u-u%s", (int)format, (unsigned)type, (unsigned)tiling, u->name);
    create_bind_image(device, mp, format, type, tiling, u->usage, one, 1u, label, c, informational);
    if (!extent_within(odd, &cap)) {
        LOG("GENERIC_ODD_SKIP: format=%d type=%u tiling=%u usage=%s reason=reported-limit max=%ux%ux%u\n",
            (int)format, (unsigned)type, (unsigned)tiling, u->name, cap.maxExtent.width, cap.maxExtent.height, cap.maxExtent.depth);
        return;
    }
    mips = full_mips(odd.width, odd.height, odd.depth);
    if (mips > cap.maxMipLevels) mips = cap.maxMipLevels;
    if (!mips) {
        mismatch(c, informational, "successful query cannot supply a valid odd mip count format=%d", (int)format);
        return;
    }
    snprintf(label, sizeof(label), "generic-odd13x7-f%d-t%u-i%u-u%s", (int)format, (unsigned)type, (unsigned)tiling, u->name);
    create_bind_image(device, mp, format, type, tiling, u->usage, odd, mips, label, c, informational);
}

static int linear_subset(VkFormat f) {
    static const VkFormat formats[] = {
        VK_FORMAT_R4G4_UNORM_PACK8, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK,
        VK_FORMAT_ASTC_12x12_SRGB_BLOCK
    };
    uint32_t i;
    for (i = 0u; i < ARRAY_LEN(formats); ++i) if (formats[i] == f) return 1;
    return 0;
}

static void run_generic_matrix(VkPhysicalDevice physical, VkDevice device, const VkPhysicalDeviceMemoryProperties *mp,
    Counts *c, int informational) {
    VkFormat f;
    uint32_t u;
    uint32_t before = c->mismatches + c->info_mismatches;
    for (f = (VkFormat)1; f <= (VkFormat)184; f = (VkFormat)(f + 1)) {
        for (u = 0u; u < ARRAY_LEN(g_usages); ++u) {
            one_generic_query(physical, device, mp, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, &g_usages[u], c, informational);
            if (linear_subset(f)) one_generic_query(physical, device, mp, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, &g_usages[u], c, informational);
        }
    }
    /* A deliberately small 1D/3D sample, using the same query-to-bind rule. */
    one_generic_query(physical, device, mp, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_1D,
        VK_IMAGE_TILING_OPTIMAL, &g_usages[0], c, informational);
    one_generic_query(physical, device, mp, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D,
        VK_IMAGE_TILING_OPTIMAL, &g_usages[0], c, informational);
    one_generic_query(physical, device, mp, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TYPE_1D,
        VK_IMAGE_TILING_OPTIMAL, &g_usages[0], c, informational);
    one_generic_query(physical, device, mp, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TYPE_3D,
        VK_IMAGE_TILING_OPTIMAL, &g_usages[0], c, informational);
    if (c->mismatches + c->info_mismatches == before) LOG("CASE PASS: generic-format-query-create-bind-matrix\n");
    else if (informational) LOG("CASE PASS: generic-format-query-create-bind-matrix informational\n");
    else LOG("CASE FAIL: generic-format-query-create-bind-matrix\n");
}

static int instance_extension_present(const char *wanted) {
    uint32_t n = 0u, i;
    VkExtensionProperties *props = NULL;
    int found = 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
    props = (VkExtensionProperties *)calloc(n, sizeof(*props));
    if (!props) return 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &n, props) == VK_SUCCESS)
        for (i = 0u; i < n; ++i) if (!strcmp(props[i].extensionName, wanted)) { found = 1; break; }
    free(props);
    return found;
}

static int device_extension_present(VkPhysicalDevice physical, const char *wanted) {
    uint32_t n = 0u, i;
    VkExtensionProperties *props = NULL;
    int found = 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
    props = (VkExtensionProperties *)calloc(n, sizeof(*props));
    if (!props) return 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &n, props) == VK_SUCCESS)
        for (i = 0u; i < n; ++i) if (!strcmp(props[i].extensionName, wanted)) { found = 1; break; }
    free(props);
    return found;
}

static void usage(const wchar_t *exe) {
    LOG("Usage:\n  %ls --loader C:\\absolute\\vulkan-1.dll --icd C:\\absolute\\ashesD3D11Renderer_icd.json [--allow-non-intel-test] [--informational] [--expect-no-decode-fallback]\n", exe);
    LOG("--informational records INFO-MISMATCH rather than strict failures; it is for RW_HD5500_LEGACY_CAPS=1 control runs only.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL;
    const wchar_t *icd_path = NULL;
    HMODULE loader = NULL;
    union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } loader_proc;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice *list = NULL;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties device_props;
    VkPhysicalDeviceMemoryProperties memory_props;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t count = 0u;
    uint32_t family = UINT32_MAX;
    int allow_non_intel = 0;
    int informational = 0;
    int expect_no_decode = 0;
    int i;
    int status = 1;
    const char *failure = NULL;
    Counts counts;
    memset(&device_props, 0, sizeof(device_props));
    memset(&memory_props, 0, sizeof(memory_props));
    memset(&counts, 0, sizeof(counts));
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--informational")) informational = 1;
        else if (!wcscmp(argv[i], L"--expect-no-decode-fallback")) expect_no_decode = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { usage(argv[0]); return 0; }
        else { ERR("CASE FAIL: argument-parsing\nFAIL: unrecognized or incomplete argument: %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!loader_path || !icd_path || !is_absolute_windows_path(loader_path) || !is_absolute_windows_path(icd_path)) {
        ERR("CASE FAIL: loader-contract\nFAIL: absolute --loader and --icd are mandatory.\n");
        return 2;
    }
    if (GetFileAttributesW(loader_path) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(icd_path) == INVALID_FILE_ATTRIBUTES) {
        ERR("CASE FAIL: loader-contract\nFAIL: supplied loader or ICD path is inaccessible (GetLastError=%lu).\n", GetLastError());
        return 2;
    }
    if (GetModuleHandleW(L"vulkan-1.dll")) {
        ERR("CASE FAIL: loader-contract\nFAIL: vulkan-1.dll already loaded; refusing ambiguous loader selection.\n");
        return 3;
    }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path) || !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) {
        ERR("CASE FAIL: loader-contract\nFAIL: could not set isolated ICD variables.\n");
        return 3;
    }
    LOG("CapabilityConsistencyProbe (v12 integration test)\nLOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    LOG("MODE: informational=%d expectNoDecodeFallback=%d allowNonIntelTest=%d\n", informational, expect_no_decode, allow_non_intel);
    LOG("SCOPE: Wine + wined3d synthetic adapter evidence only; not Intel HD 5500 hardware evidence.\n");
    loader = LoadLibraryW(loader_path);
    if (!loader) { failure = "LoadLibraryW(loader)"; goto cleanup; }
    loader_proc.raw = GetProcAddress(loader, "vkGetInstanceProcAddr");
    gipa = loader_proc.typed;
    if (!gipa) { failure = "vkGetInstanceProcAddr"; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion);
    LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties);
    LOAD_GLOBAL(vkCreateInstance);
    if (!p_vkCreateInstance) {
        union { FARPROC raw; PFN_vkCreateInstance typed; } direct_create;
        direct_create.raw = GetProcAddress(loader, "vkCreateInstance");
        p_vkCreateInstance = direct_create.typed;
        LOG("LOADER_GIPA_FALLBACK: vkCreateInstance was absent from vkGetInstanceProcAddr; used the selected loader DLL export directly.\n");
    }
    if (!p_vkEnumerateInstanceExtensionProperties || !p_vkCreateInstance) { failure = "global Vulkan entrypoint"; goto cleanup; }
    if (p_vkEnumerateInstanceVersion) {
        uint32_t v = VK_API_VERSION_1_0;
        VkResult r = p_vkEnumerateInstanceVersion(&v);
        LOG("RESULT: vkEnumerateInstanceVersion => %s (%d), max=%u.%u.%u\n", result_name(r), (int)r,
            VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
        if (r != VK_SUCCESS) { failure = "vkEnumerateInstanceVersion"; goto cleanup; }
    }
    {
        VkApplicationInfo app;
        VkInstanceCreateInfo ci;
        VkResult r;
        const char *extensions[1];
        uint32_t extension_count = 0u;
        memset(&app, 0, sizeof(app));
        memset(&ci, 0, sizeof(ci));
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "CapabilityConsistencyProbe";
        app.applicationVersion = 1u;
        app.pEngineName = "standalone";
        app.engineVersion = 1u;
        app.apiVersion = VK_API_VERSION_1_0;
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        if (instance_extension_present(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            extensions[extension_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            LOG("PORTABILITY: enabling advertised VK_KHR_portability_enumeration.\n");
        }
        ci.enabledExtensionCount = extension_count;
        ci.ppEnabledExtensionNames = extensions;
        r = p_vkCreateInstance(&ci, NULL, &instance);
        LOG("RESULT: vkCreateInstance(api=1.0) => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { failure = "vkCreateInstance"; goto cleanup; }
    }
    LOAD_INSTANCE(instance, vkDestroyInstance);
    LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceFormatProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceImageFormatProperties);
    LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties);
    LOAD_INSTANCE(instance, vkCreateDevice);
    LOAD_INSTANCE(instance, vkGetDeviceProcAddr);
    if (!p_vkDestroyInstance || !p_vkEnumeratePhysicalDevices || !p_vkGetPhysicalDeviceProperties ||
        !p_vkGetPhysicalDeviceMemoryProperties || !p_vkGetPhysicalDeviceQueueFamilyProperties ||
        !p_vkGetPhysicalDeviceFormatProperties || !p_vkGetPhysicalDeviceImageFormatProperties ||
        !p_vkEnumerateDeviceExtensionProperties || !p_vkCreateDevice || !p_vkGetDeviceProcAddr) {
        failure = "instance Vulkan entrypoint";
        goto cleanup;
    }
    {
        VkResult r = p_vkEnumeratePhysicalDevices(instance, &count, NULL);
        LOG("RESULT: vkEnumeratePhysicalDevices(count) => %s (%d), count=%u\n", result_name(r), (int)r, count);
        if (r != VK_SUCCESS || !count) { failure = "physical device enumeration"; goto cleanup; }
    }
    list = (VkPhysicalDevice *)calloc(count, sizeof(*list));
    if (!list) { failure = "physical device list allocation"; goto cleanup; }
    if (p_vkEnumeratePhysicalDevices(instance, &count, list) != VK_SUCCESS) { failure = "physical device list enumeration"; goto cleanup; }
    for (i = 0; i < (int)count; ++i) {
        VkPhysicalDeviceProperties q;
        uint32_t candidate;
        p_vkGetPhysicalDeviceProperties(list[i], &q);
        LOG("ADAPTER[%d]: name=%s vendor=0x%04X device=0x%04X type=%s%s\n", i, q.deviceName, q.vendorID,
            q.deviceID, device_type_name(q.deviceType), looks_software(&q) ? " [software-like]" : "");
        if (looks_software(&q)) { LOG("ADAPTER_REJECTED: CPU/software fallback is never eligible.\n"); continue; }
        if (!allow_non_intel && q.vendorID != INTEL_VENDOR_ID) continue;
        if (!physical && find_transfer_queue(list[i], &candidate)) {
            physical = list[i];
            family = candidate;
            device_props = q;
        }
    }
    if (!physical) { failure = "eligible adapter selection"; goto cleanup; }
    LOG("SELECTED: %s vendor=0x%04X queueFamily=%u policy=%s\n", device_props.deviceName, device_props.vendorID,
        family, allow_non_intel ? "sandbox-non-Intel-hardware-allowed" : "Intel-non-software-required");
    p_vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    run_heap_checks(physical, &memory_props, &counts, informational);
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qi;
        VkDeviceCreateInfo ci;
        const char *subset = "VK_KHR_portability_subset";
        VkResult r;
        memset(&qi, 0, sizeof(qi));
        memset(&ci, 0, sizeof(ci));
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1u;
        qi.pQueuePriorities = &priority;
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = 1u;
        ci.pQueueCreateInfos = &qi;
        if (device_extension_present(physical, subset)) {
            ci.enabledExtensionCount = 1u;
            ci.ppEnabledExtensionNames = &subset;
        }
        r = p_vkCreateDevice(physical, &ci, NULL, &device);
        LOG("RESULT: vkCreateDevice => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { failure = "vkCreateDevice"; goto cleanup; }
    }
    LOAD_DEVICE(device, vkDestroyDevice);
    LOAD_DEVICE(device, vkCreateImage);
    LOAD_DEVICE(device, vkDestroyImage);
    LOAD_DEVICE(device, vkGetImageMemoryRequirements);
    LOAD_DEVICE(device, vkAllocateMemory);
    LOAD_DEVICE(device, vkFreeMemory);
    LOAD_DEVICE(device, vkBindImageMemory);
    if (!p_vkDestroyDevice || !p_vkCreateImage || !p_vkDestroyImage || !p_vkGetImageMemoryRequirements ||
        !p_vkAllocateMemory || !p_vkFreeMemory || !p_vkBindImageMemory) {
        failure = "device Vulkan entrypoint";
        goto cleanup;
    }
    run_bc_checks(physical, device, &memory_props, &counts, informational, expect_no_decode);
    run_generic_matrix(physical, device, &memory_props, &counts, informational);
    status = (counts.mismatches == 0u) ? 0 : 11;
cleanup:
    if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL);
    free(list);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    LOG("SUMMARY: queries=%u querySuccess=%u creations=%u createSuccess=%u allocations=%u binds=%u oomAccepted=%u mismatches=%u infoMismatches=%u\n",
        counts.queries, counts.query_successes, counts.creations, counts.create_successes, counts.allocations,
        counts.binds, counts.oom_accepted, counts.mismatches, counts.info_mismatches);
    if (failure) {
        ERR("CASE FAIL: setup\nFINAL: FAIL status=1; FIRST_FAILURE: %s\n", failure);
        return 1;
    }
    if (informational) {
        LOG("FINAL: PASS informational queries=%u creations=%u mismatches=%u infoMismatches=%u\n",
            counts.queries, counts.creations, counts.mismatches, counts.info_mismatches);
        return 0;
    }
    if (status != 0) {
        ERR("FINAL: FAIL status=%d queries=%u creations=%u mismatches=%u\n", status, counts.queries,
            counts.creations, counts.mismatches);
        return status;
    }
    LOG("FINAL: PASS queries=%u creations=%u mismatches=0\n", counts.queries, counts.creations);
    return 0;
}
