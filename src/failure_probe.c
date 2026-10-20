/*
 * Windows x64 raw-Vulkan failure-path diagnostic.
 *
 * All Vulkan entry points are obtained dynamically from the caller-supplied
 * loader.  There is deliberately no Vulkan import library and VK_NO_PROTOTYPES
 * prevents accidental static Vulkan entry-point calls.
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
#include <malloc.h>

#define INTEL_VENDOR_ID 0x8086u
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define ERR(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

typedef struct AllocTracker {
    unsigned long long allocation_calls;
    unsigned long long free_calls;
    unsigned long long reallocation_calls;
} AllocTracker;

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
static PFN_vkCreateImage p_vkCreateImage;
static PFN_vkDestroyImage p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindImageMemory p_vkBindImageMemory;
static PFN_vkCreateImageView p_vkCreateImageView;
static PFN_vkDestroyImageView p_vkDestroyImageView;

#define LOAD_GLOBAL(n) p_##n = (PFN_##n)gipa(VK_NULL_HANDLE, #n)
#define LOAD_INSTANCE(i, n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d, n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)
#define NEED(n) do { \
    if (!p_##n) { ERR("FAIL: required entry point unavailable: %s\n", #n); \
        status = 20; first_failure = #n " unavailable"; goto cleanup; } \
} while (0)

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
    uint32_t count = 0, i;
    VkExtensionProperties *list = NULL;
    int found = 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) return 0;
    list = count ? (VkExtensionProperties *)calloc(count, sizeof(*list)) : NULL;
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
    list = count ? (VkExtensionProperties *)calloc(count, sizeof(*list)) : NULL;
    if (count && !list) return 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, list) == VK_SUCCESS)
        for (i = 0; i < count; ++i)
            if (!strcmp(list[i].extensionName, wanted)) { found = 1; break; }
    free(list);
    return found;
}

static int find_queue_family(VkPhysicalDevice physical, uint32_t *out_family) {
    uint32_t count = 0, i;
    VkQueueFamilyProperties *families;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    families = count ? (VkQueueFamilyProperties *)calloc(count, sizeof(*families)) : NULL;
    if (!families) return 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    for (i = 0; i < count; ++i) {
        LOG("QUEUE[%u]: flags=0x%X count=%u\n", i, families[i].queueFlags, families[i].queueCount);
        if (families[i].queueCount) {
            *out_family = i;
            free(families);
            return 1;
        }
    }
    free(families);
    return 0;
}

static int memory_type(uint32_t bits, const VkPhysicalDeviceMemoryProperties *properties,
                       VkMemoryPropertyFlags required) {
    uint32_t i;
    for (i = 0; i < properties->memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (properties->memoryTypes[i].propertyFlags & required) == required)
            return (int)i;
    return -1;
}

static size_t acceptable_alignment(size_t alignment) {
    size_t value = alignment < sizeof(void *) ? sizeof(void *) : alignment;
    size_t power = sizeof(void *);
    /* Vulkan supplies a power of two, but do not pass an invalid value to CRT. */
    while (power < value && power <= ((size_t)-1) / 2) power <<= 1;
    return power < value ? value : power;
}

/* Exact VkAllocationCallbacks ABI: VKAPI_CALL expands to the platform ABI. */
static void *VKAPI_CALL refusing_allocate(void *user_data, size_t size, size_t alignment,
                                         VkSystemAllocationScope allocation_scope) {
    (void)user_data; (void)size; (void)alignment; (void)allocation_scope;
    return NULL;
}

static void *VKAPI_CALL tracked_allocate(void *user_data, size_t size, size_t alignment,
                                         VkSystemAllocationScope allocation_scope) {
    AllocTracker *tracker = (AllocTracker *)user_data;
    void *memory;
    (void)allocation_scope;
    if (!size) size = 1;
    memory = _aligned_malloc(size, acceptable_alignment(alignment));
    if (memory) tracker->allocation_calls++;
    return memory;
}

static void *VKAPI_CALL tracked_reallocate(void *user_data, void *original, size_t size,
                                           size_t alignment, VkSystemAllocationScope allocation_scope) {
    AllocTracker *tracker = (AllocTracker *)user_data;
    void *memory;
    (void)allocation_scope;
    tracker->reallocation_calls++;
    if (!size) {
        if (original) {
            _aligned_free(original);
            tracker->free_calls++;
        }
        return NULL;
    }
    memory = _aligned_realloc(original, size, acceptable_alignment(alignment));
    if (memory && !original) tracker->allocation_calls++;
    return memory;
}

static void VKAPI_CALL tracked_free(void *user_data, void *memory) {
    AllocTracker *tracker = (AllocTracker *)user_data;
    if (memory) {
        _aligned_free(memory);
        tracker->free_calls++;
    }
}

static void init_tracking_allocator(VkAllocationCallbacks *callbacks, AllocTracker *tracker) {
    memset(tracker, 0, sizeof(*tracker));
    memset(callbacks, 0, sizeof(*callbacks));
    callbacks->pUserData = tracker;
    callbacks->pfnAllocation = tracked_allocate;
    callbacks->pfnReallocation = tracked_reallocate;
    callbacks->pfnFree = tracked_free;
}

static int tracker_balanced(const AllocTracker *tracker) {
    return tracker->allocation_calls > 0 && tracker->allocation_calls == tracker->free_calls;
}

static void init_image_create_info(VkImageCreateInfo *info) {
    memset(info, 0, sizeof(*info));
    info->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info->imageType = VK_IMAGE_TYPE_2D;
    info->format = VK_FORMAT_R8G8B8A8_UNORM;
    info->extent.width = 8;
    info->extent.height = 8;
    info->extent.depth = 1;
    info->mipLevels = 1;
    info->arrayLayers = 1;
    info->samples = VK_SAMPLE_COUNT_1_BIT;
    info->tiling = VK_IMAGE_TILING_OPTIMAL;
    info->usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    info->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

static void init_image_view_create_info(VkImageViewCreateInfo *info, VkImage image) {
    memset(info, 0, sizeof(*info));
    info->sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info->image = image;
    info->viewType = VK_IMAGE_VIEW_TYPE_2D;
    info->format = VK_FORMAT_R8G8B8A8_UNORM;
    info->components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    info->components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    info->components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    info->components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    info->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info->subresourceRange.baseMipLevel = 0;
    info->subresourceRange.levelCount = 1;
    info->subresourceRange.baseArrayLayer = 0;
    info->subresourceRange.layerCount = 1;
}

static void usage(const wchar_t *exe) {
    LOG("Usage:\n  %ls --loader C:\\absolute\\vulkan-1.dll --icd C:\\absolute\\ashesD3D11Renderer_icd.json [--allow-non-intel-test]\n", exe);
    LOG("All testing uses the supplied loader and ICD only. Default requires a non-software Intel adapter.\n");
    LOG("--allow-non-intel-test accepts Wine/synthetic/non-Intel only for sandbox integration; it is NOT Intel validation.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL;
    int allow_non_intel = 0, i, status = 1;
    const char *first_failure = NULL;
    HMODULE loader = NULL;
    union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } proc;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice *devices = NULL, physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties selected;
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint32_t device_count = 0, family = UINT32_MAX;
    VkDevice device = VK_NULL_HANDLE;
    VkImageCreateInfo image_info;
    VkImageViewCreateInfo view_info;
    VkAllocationCallbacks refusal_callbacks;
    VkImage refused_image = VK_NULL_HANDLE;
    VkResult result;
    AllocTracker failed_view_tracker, healthy_tracker;
    VkAllocationCallbacks tracked_callbacks;
    VkImageView failed_view = VK_NULL_HANDLE;
    VkImage healthy_image = VK_NULL_HANDLE;
    VkDeviceMemory healthy_memory = VK_NULL_HANDLE;
    VkImageView healthy_view = VK_NULL_HANDLE;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation_info;
    int allocation_refusal_pass = 0, failed_constructor_pass = 0, healthy_pass = 0;

    memset(&selected, 0, sizeof(selected));
    memset(&memory_properties, 0, sizeof(memory_properties));
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { usage(argv[0]); return 0; }
        else { ERR("FAIL: unrecognized or incomplete argument: %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!loader_path || !icd_path || !absolute_windows_path(loader_path) || !absolute_windows_path(icd_path)) {
        ERR("FAIL: --loader and --icd are mandatory absolute Windows paths.\n");
        usage(argv[0]);
        return 2;
    }
    if (GetFileAttributesW(loader_path) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(icd_path) == INVALID_FILE_ATTRIBUTES) {
        ERR("FAIL: supplied loader or ICD path does not exist (GetLastError=%lu).\n", GetLastError());
        return 2;
    }
    if (GetModuleHandleW(L"vulkan-1.dll")) {
        ERR("FAIL: vulkan-1.dll is already loaded; refusing ambiguous supplied-loader isolation.\n");
        return 3;
    }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path) ||
        !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) {
        ERR("FAIL: cannot set per-process ICD environment (GetLastError=%lu).\n", GetLastError());
        return 3;
    }

    LOG("Raw Vulkan failure-path probe: supplied loader/ICD only; no Vulkan import library.\n");
    LOG("LOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    if (allow_non_intel)
        LOG("WARNING: --allow-non-intel-test is sandbox-only and is NOT Intel hardware validation.\n");

    loader = LoadLibraryW(loader_path);
    if (!loader) { status = 4; first_failure = "LoadLibraryW(loader)"; ERR("FAIL: LoadLibraryW error %lu\n", GetLastError()); goto cleanup; }
    proc.raw = GetProcAddress(loader, "vkGetInstanceProcAddr");
    gipa = proc.typed;
    if (!gipa) { status = 4; first_failure = "vkGetInstanceProcAddr"; ERR("FAIL: supplied loader exposes no vkGetInstanceProcAddr.\n"); goto cleanup; }

    LOAD_GLOBAL(vkEnumerateInstanceVersion);
    LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties);
    LOAD_GLOBAL(vkCreateInstance);
    NEED(vkEnumerateInstanceExtensionProperties);
    NEED(vkCreateInstance);
    if (p_vkEnumerateInstanceVersion) {
        uint32_t version = VK_API_VERSION_1_0;
        LOG("CALL: vkEnumerateInstanceVersion\n");
        if (!checked(p_vkEnumerateInstanceVersion(&version), "vkEnumerateInstanceVersion")) {
            status = 21; first_failure = "vkEnumerateInstanceVersion"; goto cleanup;
        }
        LOG("LOADER MAX API: %u.%u.%u; application requests 1.0.0\n",
            VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    }
    {
        VkApplicationInfo application;
        VkInstanceCreateInfo create_info;
        const char *extensions[1];
        uint32_t extension_count = 0;
        memset(&application, 0, sizeof(application));
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "failure-path raw probe";
        application.applicationVersion = 1;
        application.pEngineName = "none";
        application.engineVersion = 1;
        application.apiVersion = VK_API_VERSION_1_0;
        memset(&create_info, 0, sizeof(create_info));
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &application;
        if (instance_extension_present(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            extensions[extension_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            LOG("PORTABILITY: enabling advertised VK_KHR_portability_enumeration.\n");
        }
        create_info.enabledExtensionCount = extension_count;
        create_info.ppEnabledExtensionNames = extensions;
        LOG("CALL: vkCreateInstance(api=1.0)\n");
        if (!checked(p_vkCreateInstance(&create_info, NULL, &instance), "vkCreateInstance")) {
            status = 21; first_failure = "vkCreateInstance"; goto cleanup;
        }
    }

    LOAD_INSTANCE(instance, vkDestroyInstance);
    LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceFormatProperties);
    LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties);
    /* Must be loaded before any LOAD_DEVICE use; the reference blit probe omitted this. */
    LOAD_INSTANCE(instance, vkGetDeviceProcAddr);
    LOAD_INSTANCE(instance, vkCreateDevice);
    NEED(vkDestroyInstance);
    NEED(vkEnumeratePhysicalDevices);
    NEED(vkGetPhysicalDeviceProperties);
    NEED(vkGetPhysicalDeviceMemoryProperties);
    NEED(vkGetPhysicalDeviceQueueFamilyProperties);
    NEED(vkGetPhysicalDeviceFormatProperties);
    NEED(vkEnumerateDeviceExtensionProperties);
    NEED(vkGetDeviceProcAddr);
    NEED(vkCreateDevice);

    LOG("CALL: vkEnumeratePhysicalDevices(count)\n");
    if (!checked(p_vkEnumeratePhysicalDevices(instance, &device_count, NULL), "vkEnumeratePhysicalDevices(count)")) {
        status = 21; first_failure = "vkEnumeratePhysicalDevices(count)"; goto cleanup;
    }
    if (!device_count) { status = 5; first_failure = "zero adapters"; ERR("FAIL: ICD reports no physical devices.\n"); goto cleanup; }
    devices = (VkPhysicalDevice *)calloc(device_count, sizeof(*devices));
    if (!devices) { status = 5; first_failure = "adapter list allocation"; goto cleanup; }
    LOG("CALL: vkEnumeratePhysicalDevices(list)\n");
    if (!checked(p_vkEnumeratePhysicalDevices(instance, &device_count, devices), "vkEnumeratePhysicalDevices(list)")) {
        status = 21; first_failure = "vkEnumeratePhysicalDevices(list)"; goto cleanup;
    }
    for (i = 0; i < (int)device_count; ++i) {
        VkPhysicalDeviceProperties properties;
        uint32_t found_family;
        p_vkGetPhysicalDeviceProperties(devices[i], &properties);
        LOG("ADAPTER[%d]: %s vendor=0x%04X type=%s%s\n", i, properties.deviceName, properties.vendorID,
            device_type_name(properties.deviceType), software_like(&properties) ? " [software-like]" : "");
        if (!allow_non_intel && (properties.vendorID != INTEL_VENDOR_ID || software_like(&properties))) continue;
        if (!physical && find_queue_family(devices[i], &found_family)) {
            physical = devices[i];
            family = found_family;
            selected = properties;
        }
    }
    if (!physical) {
        status = 6; first_failure = "eligible Intel adapter selection";
        ERR("FAIL: no eligible adapter. Default requires non-software Intel; --allow-non-intel-test is sandbox-only.\n");
        goto cleanup;
    }
    if (selected.vendorID != INTEL_VENDOR_ID || software_like(&selected))
        LOG("WARNING: selected non-Intel/software-like adapter; result is sandbox-only, NOT Intel validation.\n");
    LOG("SELECTED: %s vendor=0x%04X queueFamily=%u\n", selected.deviceName, selected.vendorID, family);
    p_vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    {
        VkFormatProperties format_properties;
        p_vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
        LOG("KNOWN VALID FORMAT: RGBA8_UNORM optimalFeatures=0x%X sampled-needed=0x%X\n",
            format_properties.optimalTilingFeatures, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        if (!(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            status = 7; first_failure = "RGBA8 sampled format feature";
            ERR("FAIL: RGBA8_UNORM optimal tiling does not support sampled image usage; refusing a substitute.\n");
            goto cleanup;
        }
    }
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info;
        VkDeviceCreateInfo create_info;
        const char *subset = "VK_KHR_portability_subset";
        memset(&queue_info, 0, sizeof(queue_info));
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        memset(&create_info, 0, sizeof(create_info));
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_info;
        if (device_extension_present(physical, subset)) {
            create_info.enabledExtensionCount = 1;
            create_info.ppEnabledExtensionNames = &subset;
            LOG("PORTABILITY: enabling advertised VK_KHR_portability_subset.\n");
        }
        LOG("CALL: vkCreateDevice\n");
        if (!checked(p_vkCreateDevice(physical, &create_info, NULL, &device), "vkCreateDevice")) {
            status = 21; first_failure = "vkCreateDevice"; goto cleanup;
        }
    }

    LOAD_DEVICE(device, vkDestroyDevice);
    LOAD_DEVICE(device, vkCreateImage);
    LOAD_DEVICE(device, vkDestroyImage);
    LOAD_DEVICE(device, vkGetImageMemoryRequirements);
    LOAD_DEVICE(device, vkAllocateMemory);
    LOAD_DEVICE(device, vkFreeMemory);
    LOAD_DEVICE(device, vkBindImageMemory);
    LOAD_DEVICE(device, vkCreateImageView);
    LOAD_DEVICE(device, vkDestroyImageView);
    NEED(vkDestroyDevice);
    NEED(vkCreateImage);
    NEED(vkDestroyImage);
    NEED(vkGetImageMemoryRequirements);
    NEED(vkAllocateMemory);
    NEED(vkFreeMemory);
    NEED(vkBindImageMemory);
    NEED(vkCreateImageView);
    NEED(vkDestroyImageView);

    init_image_create_info(&image_info);

    /* Deliberate allocator refusal. The create-info itself is fully valid. */
    init_tracking_allocator(&refusal_callbacks, &failed_view_tracker);
    refusal_callbacks.pfnAllocation = refusing_allocate;
    refused_image = VK_NULL_HANDLE;
    LOG("CALL: vkCreateImage(valid RGBA8 8x8x1 mip=1 layer=1 SAMPLED; valid allocator callback returns NULL)\n");
    result = p_vkCreateImage(device, &image_info, &refusal_callbacks, &refused_image);
    LOG("RESULT: vkCreateImage(refusing allocator) => %s (%d), output=%p\n",
        result_name(result), (int)result, (void *)(uintptr_t)refused_image);
    if (result != VK_ERROR_OUT_OF_HOST_MEMORY || refused_image != VK_NULL_HANDLE) {
        status = 30; first_failure = "allocation refusal was not preserved";
        ERR("FAIL: expected VK_ERROR_OUT_OF_HOST_MEMORY and VK_NULL_HANDLE output for pfnAllocation=NULL.\n");
        goto cleanup;
    }
    allocation_refusal_pass = 1;
    LOG("PASS: allocation refusal preserved\n");

    /* Deliberate defensive bad input, not a valid Vulkan conformance case. */
    init_tracking_allocator(&tracked_callbacks, &failed_view_tracker);
    init_image_view_create_info(&view_info, VK_NULL_HANDLE);
    failed_view = VK_NULL_HANDLE;
    LOG("CALL: vkCreateImageView(image=NULL deliberate defensive bad input; tracked allocator)\n");
    result = p_vkCreateImageView(device, &view_info, &tracked_callbacks, &failed_view);
    LOG("RESULT: vkCreateImageView(NULL image) => %s (%d), output=%p; callbacks alloc=%llu free=%llu realloc=%llu\n",
        result_name(result), (int)result, (void *)(uintptr_t)failed_view,
        failed_view_tracker.allocation_calls, failed_view_tracker.free_calls, failed_view_tracker.reallocation_calls);
    if (result >= 0 || failed_view != VK_NULL_HANDLE || !tracker_balanced(&failed_view_tracker)) {
        status = 31; first_failure = "failed ImageView constructor did not clean tracked storage";
        ERR("FAIL: expected negative result, NULL output, and balanced nonzero wrapper allocation/free counts.\n");
        goto cleanup;
    }
    failed_constructor_pass = 1;
    LOG("PASS: failed constructor storage freed (wrapper alloc=%llu free=%llu; no exact count assumed for internals)\n",
        failed_view_tracker.allocation_calls, failed_view_tracker.free_calls);

    /* Healthy resource path after both failures, using a fresh tracked allocator. */
    init_tracking_allocator(&tracked_callbacks, &healthy_tracker);
    healthy_image = VK_NULL_HANDLE;
    LOG("CALL: vkCreateImage(valid RGBA8 8x8x1 mip=1 layer=1 SAMPLED; tracked allocator)\n");
    result = p_vkCreateImage(device, &image_info, &tracked_callbacks, &healthy_image);
    if (!checked(result, "vkCreateImage(healthy)") || healthy_image == VK_NULL_HANDLE) {
        status = 32; first_failure = "healthy vkCreateImage"; goto cleanup;
    }
    p_vkGetImageMemoryRequirements(device, healthy_image, &requirements);
    LOG("REQUIREMENTS: size=%llu alignment=%llu typeBits=0x%08X\n",
        (unsigned long long)requirements.size, (unsigned long long)requirements.alignment, requirements.memoryTypeBits);
    {
        int type = memory_type(requirements.memoryTypeBits, &memory_properties, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) {
            status = 32; first_failure = "healthy image DEVICE_LOCAL memory type";
            ERR("FAIL: valid sampled image has no DEVICE_LOCAL memory type; no fallback substituted.\n");
            goto cleanup;
        }
        memset(&allocation_info, 0, sizeof(allocation_info));
        allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation_info.allocationSize = requirements.size;
        allocation_info.memoryTypeIndex = (uint32_t)type;
        healthy_memory = VK_NULL_HANDLE;
        LOG("CALL: vkAllocateMemory(healthy image DEVICE_LOCAL type=%d)\n", type);
        if (!checked(p_vkAllocateMemory(device, &allocation_info, &tracked_callbacks, &healthy_memory), "vkAllocateMemory(healthy)")) {
            status = 32; first_failure = "healthy vkAllocateMemory"; goto cleanup;
        }
    }
    LOG("CALL: vkBindImageMemory(healthy image)\n");
    if (!checked(p_vkBindImageMemory(device, healthy_image, healthy_memory, 0), "vkBindImageMemory(healthy)")) {
        status = 32; first_failure = "healthy vkBindImageMemory"; goto cleanup;
    }
    init_image_view_create_info(&view_info, healthy_image);
    healthy_view = VK_NULL_HANDLE;
    LOG("CALL: vkCreateImageView(healthy bound image; tracked allocator)\n");
    if (!checked(p_vkCreateImageView(device, &view_info, &tracked_callbacks, &healthy_view), "vkCreateImageView(healthy)") ||
        healthy_view == VK_NULL_HANDLE) {
        status = 32; first_failure = "healthy vkCreateImageView"; goto cleanup;
    }
    LOG("CALL: vkDestroyImageView(healthy)\n");
    p_vkDestroyImageView(device, healthy_view, &tracked_callbacks);
    healthy_view = VK_NULL_HANDLE;
    LOG("CALL: vkFreeMemory(healthy)\n");
    p_vkFreeMemory(device, healthy_memory, &tracked_callbacks);
    healthy_memory = VK_NULL_HANDLE;
    LOG("CALL: vkDestroyImage(healthy)\n");
    p_vkDestroyImage(device, healthy_image, &tracked_callbacks);
    healthy_image = VK_NULL_HANDLE;
    LOG("TRACKED healthy callbacks: alloc=%llu free=%llu realloc=%llu\n",
        healthy_tracker.allocation_calls, healthy_tracker.free_calls, healthy_tracker.reallocation_calls);
    if (!tracker_balanced(&healthy_tracker)) {
        status = 33; first_failure = "healthy resource callback balance";
        ERR("FAIL: healthy resource custom allocation/free wrapper counts are not balanced.\n");
        goto cleanup;
    }
    healthy_pass = 1;
    LOG("PASS: healthy resource after failures\n");
    status = 0;

cleanup:
    /* If a success-stage assertion failed, preserve callback pairing during cleanup. */
    if (healthy_view && device && p_vkDestroyImageView) p_vkDestroyImageView(device, healthy_view, &tracked_callbacks);
    if (healthy_memory && device && p_vkFreeMemory) p_vkFreeMemory(device, healthy_memory, &tracked_callbacks);
    if (healthy_image && device && p_vkDestroyImage) p_vkDestroyImage(device, healthy_image, &tracked_callbacks);
    if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL);
    free(devices);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    if (status) {
        ERR("FINAL: FAILURE-PATH FAIL status=%d; FIRST_FAILURE: %s\n", status,
            first_failure ? first_failure : "unspecified");
    } else if (allocation_refusal_pass && failed_constructor_pass && healthy_pass) {
        LOG("FINAL: FAILURE-PATH PASS\n");
    } else {
        ERR("FINAL: FAILURE-PATH FAIL status=34; a required pass marker was not reached.\n");
        return 34;
    }
    return status;
}
