/*
 * Raw Vulkan image create/allocate/bind diagnostic matrix.
 *
 * This is a standalone derivative of the texture-upload probe's loader/ICD
 * isolation and adapter setup.  --matrix-only deliberately stops before any
 * buffer upload, command recording, or queue submission.  It tests only
 * optimal 2D image create + memory allocation + image binding.
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
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkCreateDebugUtilsMessengerEXT p_vkCreateDebugUtilsMessengerEXT;
static PFN_vkDestroyDebugUtilsMessengerEXT p_vkDestroyDebugUtilsMessengerEXT;
static PFN_vkDestroyDevice p_vkDestroyDevice;
static PFN_vkCreateImage p_vkCreateImage;
static PFN_vkDestroyImage p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindImageMemory p_vkBindImageMemory;
static PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle;

#define LOAD_GLOBAL(n) p_##n = (PFN_##n)gipa(VK_NULL_HANDLE, #n)
#define LOAD_INSTANCE(i,n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d,n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)

static LONG g_debug_total;
static LONG g_debug_errors;
static LONG g_debug_suspect_text;
static const char *g_current_operation;
static const char *g_first_debug_operation;

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
        for (i = 0; i < n && text[i] &&
             tolower((unsigned char)text[i]) == tolower((unsigned char)part[i]); ++i) {}
        if (i == n) return 1;
    }
    return 0;
}

static int looks_software(const VkPhysicalDeviceProperties *p) {
    return p->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
        ci_contains(p->deviceName, "warp") || ci_contains(p->deviceName, "swiftshader") ||
        ci_contains(p->deviceName, "lavapipe") || ci_contains(p->deviceName, "llvmpipe") ||
        ci_contains(p->deviceName, "software") || ci_contains(p->deviceName, "basic render");
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

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user_data) {
    LONG number = InterlockedIncrement(&g_debug_total);
    const char *id_name = (data && data->pMessageIdName) ? data->pMessageIdName : "<none>";
    const char *message = (data && data->pMessage) ? data->pMessage : "<none>";
    (void)user_data;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        InterlockedIncrement(&g_debug_errors);
        if (!g_first_debug_operation)
            g_first_debug_operation = g_current_operation ? g_current_operation : "operation not tagged";
    }
    if (!text_is_sane(id_name) || !text_is_sane(message) ||
        ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) && (!data || !data->pMessage || !data->pMessage[0])))
        InterlockedIncrement(&g_debug_suspect_text);
    LOG("DEBUG_UTILS[%ld]: severity=0x%X type=0x%X code=%d id=%s message=%s\n",
        number, (unsigned)severity, (unsigned)types, data ? data->messageIdNumber : 0, id_name, message);
    return VK_FALSE;
}

static int instance_extension_present(const char *wanted) {
    uint32_t count = 0, i;
    VkExtensionProperties *list = NULL;
    int found = 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) return 0;
    if (count) list = (VkExtensionProperties *)calloc(count, sizeof(*list));
    if (count && !list) return 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, list) == VK_SUCCESS)
        for (i = 0; i < count; ++i)
            if (!strcmp(list[i].extensionName, wanted)) { found = 1; break; }
    free(list);
    return found;
}

static int device_extension_present(VkPhysicalDevice physical, const char *wanted) {
    uint32_t count = 0, i;
    VkExtensionProperties *list = NULL;
    int found = 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS) return 0;
    if (count) list = (VkExtensionProperties *)calloc(count, sizeof(*list));
    if (count && !list) return 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, list) == VK_SUCCESS)
        for (i = 0; i < count; ++i)
            if (!strcmp(list[i].extensionName, wanted)) { found = 1; break; }
    free(list);
    return found;
}

static int find_queue(VkPhysicalDevice physical, uint32_t *family_out) {
    uint32_t count = 0, i;
    VkQueueFamilyProperties *props;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (!count) return 0;
    props = (VkQueueFamilyProperties *)calloc(count, sizeof(*props));
    if (!props) return 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, props);
    for (i = 0; i < count; ++i) {
        LOG("QUEUE[%u]: flags=0x%X count=%u\n", i, props[i].queueFlags, props[i].queueCount);
        if (props[i].queueCount) { *family_out = i; free(props); return 1; }
    }
    free(props);
    return 0;
}

static uint32_t full_mip_count(uint32_t edge) {
    uint32_t result = 1;
    while (edge > 1) { edge >>= 1; ++result; }
    return result;
}

static const char *memory_flags_name(VkMemoryPropertyFlags flags) {
    (void)flags;
    return "raw VkMemoryPropertyFlags logged numerically";
}

typedef struct FormatCase {
    const char *name;
    VkFormat format;
} FormatCase;

typedef struct UsageCase {
    const char *name;
    VkImageUsageFlags usage;
} UsageCase;

static const FormatCase g_formats[] = {
    { "R8_UNORM", VK_FORMAT_R8_UNORM },
    { "R8G8_UNORM", VK_FORMAT_R8G8_UNORM },
    { "R8G8B8_UNORM", VK_FORMAT_R8G8B8_UNORM },
    { "R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM },
    { "R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB },
    { "B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM },
    { "B8G8R8A8_SRGB", VK_FORMAT_B8G8R8A8_SRGB },
    { "BC1_RGBA_UNORM_BLOCK", VK_FORMAT_BC1_RGBA_UNORM_BLOCK },
    { "BC3_UNORM_BLOCK", VK_FORMAT_BC3_UNORM_BLOCK }
};

static const UsageCase g_usages[] = {
    { "SAMPLED", VK_IMAGE_USAGE_SAMPLED_BIT },
    { "SAMPLED|TRANSFER_SRC|TRANSFER_DST", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT }
};

typedef struct MatrixCounters {
    unsigned long format_queries;
    unsigned long pure_unsupported_queries;
    unsigned long query_failures;
    unsigned long capability_limit_negatives;
    unsigned long image_create_attempts;
    unsigned long image_create_failures;
    unsigned long images_created;
    unsigned long bind_attempts;
    unsigned long alloc_failures;
    unsigned long bind_failures;
    unsigned long no_device_local;
    unsigned long no_host_visible_coherent;
} MatrixCounters;

/* Every memory type gets a fresh image. Memory is freed only after that image is destroyed. */
static void bind_one(VkDevice device, const VkImageCreateInfo *info,
                     const VkPhysicalDeviceMemoryProperties *memory_props, uint32_t type_index,
                     const char *class_name, const char *format_name, VkFormat format,
                     uint32_t dim, uint32_t mips, const char *usage_name, MatrixCounters *counts) {
    VkMemoryAllocateInfo alloc;
    VkImage image = VK_NULL_HANDLE;
    VkMemoryRequirements req;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult r;
    ++counts->image_create_attempts;
    LOG("CREATE_BIND_ATTEMPT: fresh image format=%s(%d) dim=%ux%u mips=%u usage=%s class=%s memoryType=%u\n",
        format_name, (int)format, dim, dim, mips, usage_name, class_name, type_index);
    g_current_operation = "vkCreateImage(matrix bind image)";
    r = p_vkCreateImage(device, info, NULL, &image);
    g_current_operation = NULL;
    LOG("CREATE_BIND_RESULT: format=%s dim=%u mips=%u usage=%s class=%s memoryType=%u => %s (%d)\n",
        format_name, dim, mips, usage_name, class_name, type_index, result_name(r), (int)r);
    if (r != VK_SUCCESS) {
        ++counts->image_create_failures;
        ERR("CREATE_FAILURE: a prior image-format query succeeded, so this is not pure unsupported API.\n");
        return;
    }
    ++counts->images_created;
    memset(&req, 0, sizeof(req));
    p_vkGetImageMemoryRequirements(device, image, &req);
    memset(&alloc, 0, sizeof(alloc));
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type_index;
    ++counts->bind_attempts;
    LOG("BIND_ATTEMPT: format=%s(%d) dim=%ux%u mips=%u usage=%s class=%s memoryType=%u flags=0x%X reqSize=%llu alignment=%llu typeBits=0x%08X\n",
        format_name, (int)format, dim, dim, mips, usage_name, class_name, type_index,
        memory_props->memoryTypes[type_index].propertyFlags, (unsigned long long)req.size,
        (unsigned long long)req.alignment, req.memoryTypeBits);
    g_current_operation = "vkAllocateMemory(matrix image)";
    r = p_vkAllocateMemory(device, &alloc, NULL, &memory);
    g_current_operation = NULL;
    LOG("BIND_RESULT: vkAllocateMemory format=%s dim=%u mips=%u usage=%s class=%s memoryType=%u => %s (%d)\n",
        format_name, dim, mips, usage_name, class_name, type_index, result_name(r), (int)r);
    if (r != VK_SUCCESS) { ++counts->alloc_failures; p_vkDestroyImage(device, image, NULL); return; }
    g_current_operation = "vkBindImageMemory(matrix image)";
    r = p_vkBindImageMemory(device, image, memory, 0);
    g_current_operation = NULL;
    LOG("BIND_RESULT: vkBindImageMemory format=%s(%d) dim=%u mips=%u usage=%s class=%s memoryType=%u => %s (%d)\n",
        format_name, (int)format, dim, mips, usage_name, class_name, type_index, result_name(r), (int)r);
    if (r != VK_SUCCESS) ++counts->bind_failures;
    p_vkDestroyImage(device, image, NULL);
    p_vkFreeMemory(device, memory, NULL);
}

static void run_matrix(VkPhysicalDevice physical, VkDevice device,
                       const VkPhysicalDeviceMemoryProperties *memory_props,
                       MatrixCounters *counts) {
    static const uint32_t dims[] = { 1u, 2u, 4u, 16u };
    size_t fi, ui, di, mi;
    for (fi = 0; fi < sizeof(g_formats) / sizeof(g_formats[0]); ++fi) {
        for (ui = 0; ui < sizeof(g_usages) / sizeof(g_usages[0]); ++ui) {
            VkImageFormatProperties cap;
            VkResult qr;
            memset(&cap, 0, sizeof(cap));
            ++counts->format_queries;
            LOG("FORMAT_QUERY: format=%s(%d) type=2D tiling=OPTIMAL usage=%s flags=0x0\n",
                g_formats[fi].name, (int)g_formats[fi].format, g_usages[ui].name);
            g_current_operation = "vkGetPhysicalDeviceImageFormatProperties";
            qr = p_vkGetPhysicalDeviceImageFormatProperties(physical, g_formats[fi].format,
                VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, g_usages[ui].usage, 0, &cap);
            g_current_operation = NULL;
            LOG("FORMAT_QUERY_RESULT: format=%s usage=%s => %s (%d); maxExtent=%ux%ux%u maxMipLevels=%u maxArrayLayers=%u sampleCounts=0x%X maxResourceSize=%llu\n",
                g_formats[fi].name, g_usages[ui].name, result_name(qr), (int)qr,
                cap.maxExtent.width, cap.maxExtent.height, cap.maxExtent.depth, cap.maxMipLevels,
                cap.maxArrayLayers, cap.sampleCounts, (unsigned long long)cap.maxResourceSize);
            if (qr == VK_ERROR_FORMAT_NOT_SUPPORTED) {
                ++counts->pure_unsupported_queries;
                LOG("CAPABILITY_NEGATIVE: pure unsupported API case; no vkCreateImage calls for format=%s usage=%s.\n",
                    g_formats[fi].name, g_usages[ui].name);
                continue;
            }
            if (qr != VK_SUCCESS) {
                ++counts->query_failures;
                ERR("QUERY_FAILURE: unexpected image-format-property result; no blind create for format=%s usage=%s.\n",
                    g_formats[fi].name, g_usages[ui].name);
                continue;
            }
            for (di = 0; di < sizeof(dims) / sizeof(dims[0]); ++di) {
                for (mi = 0; mi < 2; ++mi) {
                    VkImageCreateInfo info;
                    VkImage image = VK_NULL_HANDLE;
                    VkMemoryRequirements req;
                    VkResult cr;
                    uint32_t mips = mi ? full_mip_count(dims[di]) : 1u;
                    uint32_t type;
                    int have_device_local = 0;
                    int have_host_visible_coherent = 0;
                    if (dims[di] > cap.maxExtent.width || dims[di] > cap.maxExtent.height ||
                        1u > cap.maxExtent.depth || mips > cap.maxMipLevels) {
                        ++counts->capability_limit_negatives;
                        LOG("CAPABILITY_NEGATIVE: format=%s dim=%ux%u mips=%u usage=%s exceeds successful query limits; create intentionally skipped.\n",
                            g_formats[fi].name, dims[di], dims[di], mips, g_usages[ui].name);
                        continue;
                    }
                    memset(&info, 0, sizeof(info));
                    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                    info.imageType = VK_IMAGE_TYPE_2D;
                    info.format = g_formats[fi].format;
                    info.extent.width = dims[di];
                    info.extent.height = dims[di];
                    info.extent.depth = 1;
                    info.mipLevels = mips;
                    info.arrayLayers = 1;
                    info.samples = VK_SAMPLE_COUNT_1_BIT;
                    info.tiling = VK_IMAGE_TILING_OPTIMAL;
                    info.usage = g_usages[ui].usage;
                    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    ++counts->image_create_attempts;
                    LOG("CREATE_ATTEMPT: format=%s(%d) dim=%ux%u mips=%u (%s) usage=%s\n",
                        g_formats[fi].name, (int)g_formats[fi].format, dims[di], dims[di], mips,
                        mi ? "full-chain" : "base-only", g_usages[ui].name);
                    g_current_operation = "vkCreateImage(matrix image)";
                    cr = p_vkCreateImage(device, &info, NULL, &image);
                    g_current_operation = NULL;
                    LOG("CREATE_RESULT: format=%s dim=%u mips=%u usage=%s => %s (%d)\n",
                        g_formats[fi].name, dims[di], mips, g_usages[ui].name, result_name(cr), (int)cr);
                    if (cr != VK_SUCCESS) {
                        ++counts->image_create_failures;
                        ERR("CREATE_FAILURE: format query had succeeded, so this is not classified as pure unsupported API.\n");
                        continue;
                    }
                    ++counts->images_created;
                    memset(&req, 0, sizeof(req));
                    p_vkGetImageMemoryRequirements(device, image, &req);
                    LOG("IMAGE_REQUIREMENTS: format=%s dim=%u mips=%u usage=%s size=%llu alignment=%llu typeBits=0x%08X\n",
                        g_formats[fi].name, dims[di], mips, g_usages[ui].name,
                        (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits);
                    /* The probe image was only for requirements/typeBits.  A fresh image is made for each bind. */
                    p_vkDestroyImage(device, image, NULL);
                    image = VK_NULL_HANDLE;
                    for (type = 0; type < memory_props->memoryTypeCount; ++type) {
                        VkMemoryPropertyFlags flags = memory_props->memoryTypes[type].propertyFlags;
                        if (!(req.memoryTypeBits & (1u << type))) continue;
                        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                            have_device_local = 1;
                            bind_one(device, &info, memory_props, type, "DEVICE_LOCAL", g_formats[fi].name,
                                g_formats[fi].format, dims[di], mips, g_usages[ui].name, counts);
                        }
                        if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                            have_host_visible_coherent = 1;
                            bind_one(device, &info, memory_props, type, "HOST_VISIBLE|HOST_COHERENT", g_formats[fi].name,
                                g_formats[fi].format, dims[di], mips, g_usages[ui].name, counts);
                        }
                    }
                    if (!have_device_local) {
                        ++counts->no_device_local;
                        LOG("MEMTYPE_NEGATIVE: format=%s dim=%u mips=%u usage=%s class=DEVICE_LOCAL; no compatible advertised image-memory type in typeBits=0x%08X.\n",
                            g_formats[fi].name, dims[di], mips, g_usages[ui].name, req.memoryTypeBits);
                    }
                    if (!have_host_visible_coherent) {
                        ++counts->no_host_visible_coherent;
                        LOG("MEMTYPE_NEGATIVE: format=%s dim=%u mips=%u usage=%s class=HOST_VISIBLE|HOST_COHERENT; no compatible advertised image-memory type in typeBits=0x%08X.\n",
                            g_formats[fi].name, dims[di], mips, g_usages[ui].name, req.memoryTypeBits);
                    }
                }
            }
        }
    }
}

static void usage(const wchar_t *exe) {
    LOG("Usage:\n  %ls --loader C:\\absolute\\vulkan-1.dll --icd C:\\absolute\\ashesD3D11Renderer_icd.json --matrix-only [--allow-non-intel-test]\n", exe);
    LOG("--matrix-only is mandatory. The program exits after image create/allocate/bind tests and never enters an upload/copy path.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL;
    int allow_non_intel = 0, matrix_only = 0, i, status = 1;
    HMODULE loader = NULL;
    union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } loader_gipa;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    int debug_utils_enabled = 0;
    VkPhysicalDevice *physical_list = NULL, physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties selected_props;
    VkPhysicalDeviceMemoryProperties memory_props;
    uint32_t physical_count = 0, queue_family = UINT32_MAX;
    VkDevice device = VK_NULL_HANDLE;
    MatrixCounters counts;
    const char *first_failure = NULL;

    memset(&selected_props, 0, sizeof(selected_props));
    memset(&memory_props, 0, sizeof(memory_props));
    memset(&counts, 0, sizeof(counts));
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--matrix-only")) matrix_only = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { usage(argv[0]); return 0; }
        else { ERR("FAIL: unrecognized or incomplete argument: %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!loader_path || !icd_path || !matrix_only || !is_absolute_windows_path(loader_path) || !is_absolute_windows_path(icd_path)) {
        ERR("FAIL: mandatory absolute --loader, --icd, and --matrix-only arguments are required.\n"); usage(argv[0]); return 2;
    }
    if (GetFileAttributesW(loader_path) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(icd_path) == INVALID_FILE_ATTRIBUTES) {
        ERR("FAIL: supplied loader or ICD path does not exist (GetLastError=%lu).\n", GetLastError()); return 2;
    }
    if (GetModuleHandleW(L"vulkan-1.dll")) {
        ERR("FAIL: vulkan-1.dll was already loaded; refusing ambiguous ICD isolation.\n"); return 3;
    }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path) || !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) {
        ERR("FAIL: cannot set isolated per-process ICD environment (GetLastError=%lu).\n", GetLastError()); return 3;
    }

    LOG("Ashes D3D11 raw Vulkan texture create/bind matrix\n");
    LOG("MODE: --matrix-only. No upload buffer, command buffer, copy, submit, or game path will run.\n");
    LOG("LOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    LOG("ICD isolation: VK_ICD_FILENAMES and VK_DRIVER_FILES set in this process only.\n");
    if (allow_non_intel) LOG("WARNING: synthetic/non-Intel adapter allowed for Wine sandbox integration only; NOT Intel hardware validation.\n");

    loader = LoadLibraryW(loader_path);
    if (!loader) { ERR("FAIL: LoadLibraryW(loader) error %lu\n", GetLastError()); status = 4; first_failure = "LoadLibraryW(loader)"; goto cleanup; }
    loader_gipa.raw = GetProcAddress(loader, "vkGetInstanceProcAddr");
    gipa = loader_gipa.typed;
    if (!gipa) { ERR("FAIL: loader exposes no vkGetInstanceProcAddr.\n"); status = 4; first_failure = "vkGetInstanceProcAddr"; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion); LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties); LOAD_GLOBAL(vkCreateInstance);
    if (!p_vkEnumerateInstanceExtensionProperties || !p_vkCreateInstance) { ERR("FAIL: global Vulkan entry point unavailable.\n"); status = 20; first_failure = "required global entrypoint"; goto cleanup; }
    if (p_vkEnumerateInstanceVersion) {
        uint32_t version = VK_API_VERSION_1_0;
        VkResult r = p_vkEnumerateInstanceVersion(&version);
        LOG("RESULT: vkEnumerateInstanceVersion => %s (%d), loader max %u.%u.%u\n", result_name(r), (int)r,
            VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkEnumerateInstanceVersion"; goto cleanup; }
    }
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Ashes texture matrix", 1u, "standalone", 1u, VK_API_VERSION_1_0 };
        VkInstanceCreateInfo create;
        VkDebugUtilsMessengerCreateInfoEXT debug_create;
        const char *extensions[2];
        uint32_t extension_count = 0;
        VkResult r;
        memset(&create, 0, sizeof(create));
        memset(&debug_create, 0, sizeof(debug_create));
        create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create.pApplicationInfo = &app;
        if (instance_extension_present(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            create.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            extensions[extension_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            LOG("PORTABILITY: enabling advertised VK_KHR_portability_enumeration.\n");
        }
        if (instance_extension_present(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            debug_utils_enabled = 1;
            extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            debug_create.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debug_create.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug_create.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug_create.pfnUserCallback = debug_callback;
            create.pNext = &debug_create;
            LOG("DEBUG_UTILS: VK_EXT_debug_utils advertised; enabling native callback.\n");
        } else LOG("DEBUG_UTILS: not advertised; no native callback available.\n");
        create.enabledExtensionCount = extension_count;
        create.ppEnabledExtensionNames = extensions;
        r = p_vkCreateInstance(&create, NULL, &instance);
        LOG("RESULT: vkCreateInstance(api=1.0) => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateInstance"; goto cleanup; }
    }
    LOAD_INSTANCE(instance, vkDestroyInstance); LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceImageFormatProperties);
    LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties); LOAD_INSTANCE(instance, vkCreateDevice);
    LOAD_INSTANCE(instance, vkGetDeviceProcAddr); LOAD_INSTANCE(instance, vkCreateDebugUtilsMessengerEXT); LOAD_INSTANCE(instance, vkDestroyDebugUtilsMessengerEXT);
    if (!p_vkDestroyInstance || !p_vkEnumeratePhysicalDevices || !p_vkGetPhysicalDeviceProperties ||
        !p_vkGetPhysicalDeviceMemoryProperties || !p_vkGetPhysicalDeviceQueueFamilyProperties || !p_vkGetPhysicalDeviceImageFormatProperties ||
        !p_vkEnumerateDeviceExtensionProperties || !p_vkCreateDevice || !p_vkGetDeviceProcAddr) {
        ERR("FAIL: required instance Vulkan entry point unavailable.\n"); status = 20; first_failure = "required instance entrypoint"; goto cleanup;
    }
    if (debug_utils_enabled) {
        VkDebugUtilsMessengerCreateInfoEXT info;
        VkResult r;
        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = debug_callback;
        if (!p_vkCreateDebugUtilsMessengerEXT || !p_vkDestroyDebugUtilsMessengerEXT) {
            ERR("FAIL: VK_EXT_debug_utils advertised but messenger entry points unavailable.\n"); status = 20; first_failure = "debug messenger entrypoint"; goto cleanup;
        }
        r = p_vkCreateDebugUtilsMessengerEXT(instance, &info, NULL, &debug_messenger);
        LOG("RESULT: vkCreateDebugUtilsMessengerEXT => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateDebugUtilsMessengerEXT"; goto cleanup; }
    }
    {
        VkResult r = p_vkEnumeratePhysicalDevices(instance, &physical_count, NULL);
        LOG("RESULT: vkEnumeratePhysicalDevices(count) => %s (%d), count=%u\n", result_name(r), (int)r, physical_count);
        if (r != VK_SUCCESS || !physical_count) { status = 5; first_failure = "vkEnumeratePhysicalDevices"; goto cleanup; }
    }
    physical_list = (VkPhysicalDevice *)calloc(physical_count, sizeof(*physical_list));
    if (!physical_list) { status = 5; first_failure = "physical device list allocation"; goto cleanup; }
    {
        VkResult r = p_vkEnumeratePhysicalDevices(instance, &physical_count, physical_list);
        LOG("RESULT: vkEnumeratePhysicalDevices(list) => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { status = 5; first_failure = "vkEnumeratePhysicalDevices list"; goto cleanup; }
    }
    for (i = 0; i < (int)physical_count; ++i) {
        VkPhysicalDeviceProperties props;
        uint32_t candidate_family;
        p_vkGetPhysicalDeviceProperties(physical_list[i], &props);
        LOG("ADAPTER[%d]: name=%s vendor=0x%04X device=0x%04X type=%s%s\n", i, props.deviceName, props.vendorID, props.deviceID,
            device_type_name(props.deviceType), looks_software(&props) ? " [software-like]" : "");
        if (!allow_non_intel && (props.vendorID != INTEL_VENDOR_ID || looks_software(&props))) continue;
        if (!physical && find_queue(physical_list[i], &candidate_family)) {
            physical = physical_list[i]; queue_family = candidate_family; selected_props = props;
        }
    }
    if (!physical) { ERR("FAIL: no eligible queue-bearing adapter. Default requires non-software Intel; sandbox requires --allow-non-intel-test.\n"); status = 6; first_failure = "adapter selection"; goto cleanup; }
    if (selected_props.vendorID != INTEL_VENDOR_ID || looks_software(&selected_props))
        LOG("WARNING: selected synthetic/non-Intel/software-like adapter: sandbox result only, NOT a real Intel performance or hardware claim.\n");
    LOG("SELECTED: %s vendor=0x%04X queueFamily=%u\n", selected_props.deviceName, selected_props.vendorID, queue_family);
    p_vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    for (i = 0; i < (int)memory_props.memoryTypeCount; ++i)
        LOG("MEMORY_TYPE[%d]: heap=%u flags=0x%X (%s)\n", i, memory_props.memoryTypes[i].heapIndex,
            memory_props.memoryTypes[i].propertyFlags, memory_flags_name(memory_props.memoryTypes[i].propertyFlags));
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0u, queue_family, 1u, &priority };
        VkDeviceCreateInfo create = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0u, 1u, &qci, 0u, NULL, 0u, NULL, NULL };
        const char *subset = "VK_KHR_portability_subset";
        VkResult r;
        if (device_extension_present(physical, subset)) {
            create.enabledExtensionCount = 1; create.ppEnabledExtensionNames = &subset;
            LOG("PORTABILITY: enabling advertised VK_KHR_portability_subset.\n");
        }
        r = p_vkCreateDevice(physical, &create, NULL, &device);
        LOG("RESULT: vkCreateDevice => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateDevice"; goto cleanup; }
    }
    LOAD_DEVICE(device, vkDestroyDevice); LOAD_DEVICE(device, vkCreateImage); LOAD_DEVICE(device, vkDestroyImage);
    LOAD_DEVICE(device, vkGetImageMemoryRequirements); LOAD_DEVICE(device, vkAllocateMemory); LOAD_DEVICE(device, vkFreeMemory);
    LOAD_DEVICE(device, vkBindImageMemory); LOAD_DEVICE(device, vkDeviceWaitIdle);
    if (!p_vkDestroyDevice || !p_vkCreateImage || !p_vkDestroyImage || !p_vkGetImageMemoryRequirements ||
        !p_vkAllocateMemory || !p_vkFreeMemory || !p_vkBindImageMemory || !p_vkDeviceWaitIdle) {
        ERR("FAIL: required device Vulkan entry point unavailable.\n"); status = 20; first_failure = "required device entrypoint"; goto cleanup;
    }

    run_matrix(physical, device, &memory_props, &counts);
    {
        VkResult r = p_vkDeviceWaitIdle(device);
        LOG("RESULT: vkDeviceWaitIdle(matrix cleanup) => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkDeviceWaitIdle"; }
    }
    if (!status || status == 1) {
        if (g_debug_errors) { status = 10; first_failure = "VK_EXT_debug_utils ERROR callback"; }
        else if (counts.query_failures || counts.image_create_failures || counts.alloc_failures || counts.bind_failures) {
            status = 11; first_failure = "matrix create/allocate/bind diagnostic failure";
        } else status = 0;
    }

cleanup:
    LOG("MATRIX_SUMMARY: formatQueries=%lu pureUnsupportedQueries=%lu queryFailures=%lu capabilityLimitNegatives=%lu createAttempts=%lu createFailures=%lu imagesCreated=%lu bindAttempts=%lu allocFailures=%lu bindFailures=%lu noDeviceLocal=%lu noHostVisibleCoherent=%lu\n",
        counts.format_queries, counts.pure_unsupported_queries, counts.query_failures, counts.capability_limit_negatives,
        counts.image_create_attempts, counts.image_create_failures, counts.images_created, counts.bind_attempts,
        counts.alloc_failures, counts.bind_failures, counts.no_device_local, counts.no_host_visible_coherent);
    if (g_debug_errors) { status = 10; if (!first_failure) first_failure = "VK_EXT_debug_utils ERROR callback"; }
    if (device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(device);
    if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL);
    free(physical_list);
    if (debug_messenger && p_vkDestroyDebugUtilsMessengerEXT) p_vkDestroyDebugUtilsMessengerEXT(instance, debug_messenger, NULL);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    LOG("DEBUG_SUMMARY: total=%ld errors=%ld textIntegrity=%s\n", g_debug_total, g_debug_errors,
        g_debug_suspect_text ? "SUSPECT(non-printable/unterminated callback text)" : "sane-or-no-messages");
    if (g_first_debug_operation) LOG("FIRST_DEBUG_ERROR_OPERATION: %s\n", g_first_debug_operation);
    if (status) ERR("FINAL: FAIL status=%d; FIRST_FAILURE: %s\n", status, first_failure ? first_failure : "unspecified");
    else LOG("FINAL: PASS status=0; matrix completed without callback, query, create, allocation, or bind failures.\n");
    return status;
}
