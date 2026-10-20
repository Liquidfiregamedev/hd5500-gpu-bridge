/*
 * Focused Ashes D3D11 ICD shared-VkDeviceMemory buffer-bind repro.
 *
 * This is deliberately raw Vulkan, not VMA: vmaCreateBuffer ultimately reaches
 * vkBindBufferMemory, where the bridge allocates/binds its D3D11 representation.
 * Two 16 MiB TRANSFER_SRC|TRANSFER_DST VkBuffers share one host-visible,
 * host-coherent parent VkDeviceMemory allocation.  The second bind starts at
 * half the parent allocation and is the operation under test.
 *
 * The executable imports no Vulkan functions.  It sets VK_ICD_FILENAMES and
 * VK_DRIVER_FILES for itself, then LoadLibraryW()s only the caller-specified
 * absolute loader path.  It does not touch the registry, game, renderer, or
 * package files.
 */
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define INTEL_VENDOR_ID 0x8086u
#define MIB (1024ull * 1024ull)
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
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;

static PFN_vkDestroyDevice p_vkDestroyDevice;
static PFN_vkGetDeviceQueue p_vkGetDeviceQueue;
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
static PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
static PFN_vkCmdCopyBuffer p_vkCmdCopyBuffer;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkDestroyFence p_vkDestroyFence;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkWaitForFences p_vkWaitForFences;
static PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle;

#define LOAD_GLOBAL(n) p_##n = (PFN_##n)gipa(VK_NULL_HANDLE, #n)
#define LOAD_INSTANCE(i,n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d,n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)
#define NEED(n) do { if (!p_##n) { ERR("FAIL: required entry point unavailable: %s\n", #n); status = 20; goto cleanup; } } while (0)
#define VK_CALL(label, expr) do { \
    VkResult call_result_ = (expr); \
    LOG("RESULT: %s => %s (%d)\n", (label), result_name(call_result_), (int)call_result_); \
    if (call_result_ != VK_SUCCESS) { status = 21; goto cleanup; } \
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

static uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return alignment ? ((value + alignment - 1u) / alignment) * alignment : value;
}

static int instance_extension_present(const char *wanted) {
    uint32_t count = 0, i;
    VkExtensionProperties *list = NULL;
    int found = 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) return 0;
    if (count) {
        list = (VkExtensionProperties *)calloc(count, sizeof(*list));
        if (!list) return 0;
    }
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
    if (count) {
        list = (VkExtensionProperties *)calloc(count, sizeof(*list));
        if (!list) return 0;
    }
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, list) == VK_SUCCESS)
        for (i = 0; i < count; ++i)
            if (!strcmp(list[i].extensionName, wanted)) { found = 1; break; }
    free(list);
    return found;
}

static int find_transfer_queue(VkPhysicalDevice physical, uint32_t *family_out) {
    uint32_t count = 0, i;
    VkQueueFamilyProperties *props = NULL;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (!count) return 0;
    props = (VkQueueFamilyProperties *)calloc(count, sizeof(*props));
    if (!props) return 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, props);
    for (i = 0; i < count; ++i) {
        if (props[i].queueCount && (props[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
            *family_out = i;
            free(props);
            return 1;
        }
    }
    free(props);
    return 0;
}

static int choose_memory_type(uint32_t memory_type_bits,
                              const VkPhysicalDeviceMemoryProperties *memory_props) {
    uint32_t i;
    const VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (i = 0; i < memory_props->memoryTypeCount; ++i) {
        if ((memory_type_bits & (1u << i)) &&
            (memory_props->memoryTypes[i].propertyFlags & required) == required)
            return (int)i;
    }
    return -1;
}

static void write_parent_patterns(uint8_t *data, uint64_t half_block) {
    uint64_t i;
    for (i = 0; i < half_block; ++i) {
        data[i] = (uint8_t)(0xA5u ^ (uint8_t)(i * 13u));
        data[half_block + i] = (uint8_t)(0x5Au ^ (uint8_t)(i * 29u));
    }
}

static int first_half_is_second_pattern(const uint8_t *data, uint64_t half_block) {
    uint64_t samples[] = {0u, 1u, 17u, 4095u, 65537u, half_block / 2u, half_block - 1u};
    size_t i;
    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        uint64_t at = samples[i];
        uint8_t expected = (uint8_t)(0x5Au ^ (uint8_t)(at * 29u));
        if (data[at] != expected) {
            ERR("FAIL: readback sample at %llu is 0x%02X, expected second-half pattern 0x%02X\n",
                (unsigned long long)at, data[at], expected);
            return 0;
        }
    }
    return 1;
}

static void usage(const wchar_t *exe) {
    LOG("Usage:\n  %ls --loader C:\\\\absolute\\\\vulkan-1.dll --icd C:\\\\absolute\\\\ashesD3D11Renderer_icd.json [--allow-non-intel-test]\n", exe);
    LOG("Both paths are mandatory and absolute. The program sets VK_ICD_FILENAMES and VK_DRIVER_FILES only in its own process.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL;
    int allow_non_intel = 0;
    int status = 1;
    int i;
    HMODULE loader = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice *physicals = NULL;
    uint32_t physical_count = 0;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties selected_props;
    VkPhysicalDeviceMemoryProperties memory_props;
    uint32_t queue_family = UINT32_MAX;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkBuffer buffer_a = VK_NULL_HANDLE, buffer_b = VK_NULL_HANDLE;
    VkDeviceMemory parent_memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } loader_gipa;

    memset(&selected_props, 0, sizeof(selected_props));
    memset(&memory_props, 0, sizeof(memory_props));

    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { usage(argv[0]); return 0; }
        else { ERR("FAIL: unrecognized or incomplete argument: %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!loader_path || !icd_path || !is_absolute_windows_path(loader_path) || !is_absolute_windows_path(icd_path)) {
        ERR("FAIL: --loader and --icd must both be supplied as absolute existing Windows paths.\n");
        usage(argv[0]);
        return 2;
    }
    if (GetFileAttributesW(loader_path) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(icd_path) == INVALID_FILE_ATTRIBUTES) {
        ERR("FAIL: supplied loader or ICD path does not exist (GetLastError=%lu).\n", GetLastError());
        return 2;
    }
    if (GetModuleHandleW(L"vulkan-1.dll")) {
        ERR("FAIL: vulkan-1.dll was already loaded; refusing ambiguous ICD isolation.\n");
        return 3;
    }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path) ||
        !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) {
        ERR("FAIL: cannot set per-process Vulkan ICD environment (GetLastError=%lu).\n", GetLastError());
        return 3;
    }

    LOG("Ashes D3D11 ICD focused shared-memory buffer-bind repro (raw Vulkan; no VMA)\n");
    LOG("LOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    LOG("ICD isolation: VK_ICD_FILENAMES and VK_DRIVER_FILES are set in this process only.\n");
    if (allow_non_intel)
        LOG("WARNING: --allow-non-intel-test accepts a Wine/synthetic/software-backed adapter only for sandbox integration; it is NOT HD 5500 validation.\n");

    loader = LoadLibraryW(loader_path);
    if (!loader) { ERR("FAIL: LoadLibraryW(loader) error %lu\n", GetLastError()); status = 4; goto cleanup; }
    loader_gipa.raw = GetProcAddress(loader, "vkGetInstanceProcAddr");
    gipa = loader_gipa.typed;
    if (!gipa) { ERR("FAIL: loader exposes no vkGetInstanceProcAddr.\n"); status = 4; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion);
    LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties);
    LOAD_GLOBAL(vkCreateInstance);
    NEED(vkEnumerateInstanceExtensionProperties);
    NEED(vkCreateInstance);
    if (p_vkEnumerateInstanceVersion) {
        uint32_t version = VK_API_VERSION_1_0;
        VK_CALL("vkEnumerateInstanceVersion", p_vkEnumerateInstanceVersion(&version));
        LOG("LOADER MAX API: %u.%u.%u; application requests 1.0.0\n",
            VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    }
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL,
            "Ashes shared buffer bind repro", 1u, "standalone", 1u, VK_API_VERSION_1_0 };
        VkInstanceCreateInfo create = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0u,
            &app, 0u, NULL, 0u, NULL };
        const char *portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        if (instance_extension_present(portability)) {
            create.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            create.enabledExtensionCount = 1u;
            create.ppEnabledExtensionNames = &portability;
            LOG("PORTABILITY: enabling advertised VK_KHR_portability_enumeration.\n");
        }
        VK_CALL("vkCreateInstance(api=1.0)", p_vkCreateInstance(&create, NULL, &instance));
    }
    LOAD_INSTANCE(instance, vkDestroyInstance);
    LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties);
    LOAD_INSTANCE(instance, vkCreateDevice);
    LOAD_INSTANCE(instance, vkGetDeviceProcAddr);
    NEED(vkDestroyInstance); NEED(vkEnumeratePhysicalDevices); NEED(vkGetPhysicalDeviceProperties);
    NEED(vkGetPhysicalDeviceMemoryProperties); NEED(vkGetPhysicalDeviceQueueFamilyProperties);
    NEED(vkEnumerateDeviceExtensionProperties); NEED(vkCreateDevice); NEED(vkGetDeviceProcAddr);

    VK_CALL("vkEnumeratePhysicalDevices(count)", p_vkEnumeratePhysicalDevices(instance, &physical_count, NULL));
    if (!physical_count) { ERR("FAIL: ICD reports no physical devices.\n"); status = 5; goto cleanup; }
    physicals = (VkPhysicalDevice *)calloc(physical_count, sizeof(*physicals));
    if (!physicals) { ERR("FAIL: host allocation for physical-device list.\n"); status = 5; goto cleanup; }
    VK_CALL("vkEnumeratePhysicalDevices(list)", p_vkEnumeratePhysicalDevices(instance, &physical_count, physicals));
    for (i = 0; i < (int)physical_count; ++i) {
        VkPhysicalDeviceProperties props;
        uint32_t candidate_family;
        p_vkGetPhysicalDeviceProperties(physicals[i], &props);
        LOG("ADAPTER[%d]: name=%s vendor=0x%04X device=0x%04X type=%s%s\n", i,
            props.deviceName, props.vendorID, props.deviceID, device_type_name(props.deviceType),
            looks_software(&props) ? " [software-like]" : "");
        if (!allow_non_intel && (props.vendorID != INTEL_VENDOR_ID || looks_software(&props))) continue;
        if (!physical && find_transfer_queue(physicals[i], &candidate_family)) {
            physical = physicals[i];
            queue_family = candidate_family;
            selected_props = props;
        }
    }
    if (!physical) {
        ERR("FAIL: no eligible transfer-queue adapter. Without --allow-non-intel-test this requires a non-software Intel adapter.\n");
        status = 6;
        goto cleanup;
    }
    if (selected_props.vendorID != INTEL_VENDOR_ID || looks_software(&selected_props))
        LOG("WARNING: selected adapter is synthetic/non-Intel and/or software-like: sandbox integration result only, NOT HD 5500 hardware validation.\n");
    LOG("SELECTED: %s, vendor=0x%04X, queueFamily=%u\n", selected_props.deviceName, selected_props.vendorID, queue_family);
    p_vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);

    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0u, queue_family, 1u, &priority };
        VkDeviceCreateInfo create = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0u,
            1u, &qci, 0u, NULL, 0u, NULL, NULL };
        const char *portability_subset = "VK_KHR_portability_subset";
        if (device_extension_present(physical, portability_subset)) {
            create.enabledExtensionCount = 1u;
            create.ppEnabledExtensionNames = &portability_subset;
            LOG("PORTABILITY: enabling advertised VK_KHR_portability_subset.\n");
        }
        VK_CALL("vkCreateDevice", p_vkCreateDevice(physical, &create, NULL, &device));
    }
    LOAD_DEVICE(device, vkDestroyDevice); LOAD_DEVICE(device, vkGetDeviceQueue);
    LOAD_DEVICE(device, vkCreateBuffer); LOAD_DEVICE(device, vkDestroyBuffer);
    LOAD_DEVICE(device, vkGetBufferMemoryRequirements); LOAD_DEVICE(device, vkAllocateMemory);
    LOAD_DEVICE(device, vkFreeMemory); LOAD_DEVICE(device, vkBindBufferMemory);
    LOAD_DEVICE(device, vkMapMemory); LOAD_DEVICE(device, vkUnmapMemory);
    LOAD_DEVICE(device, vkFlushMappedMemoryRanges); LOAD_DEVICE(device, vkInvalidateMappedMemoryRanges);
    LOAD_DEVICE(device, vkCreateCommandPool); LOAD_DEVICE(device, vkDestroyCommandPool);
    LOAD_DEVICE(device, vkAllocateCommandBuffers); LOAD_DEVICE(device, vkBeginCommandBuffer);
    LOAD_DEVICE(device, vkEndCommandBuffer); LOAD_DEVICE(device, vkCmdCopyBuffer);
    LOAD_DEVICE(device, vkCreateFence); LOAD_DEVICE(device, vkDestroyFence);
    LOAD_DEVICE(device, vkQueueSubmit); LOAD_DEVICE(device, vkWaitForFences); LOAD_DEVICE(device, vkDeviceWaitIdle);
    NEED(vkDestroyDevice); NEED(vkGetDeviceQueue); NEED(vkCreateBuffer); NEED(vkDestroyBuffer);
    NEED(vkGetBufferMemoryRequirements); NEED(vkAllocateMemory); NEED(vkFreeMemory); NEED(vkBindBufferMemory);
    NEED(vkMapMemory); NEED(vkUnmapMemory); NEED(vkFlushMappedMemoryRanges); NEED(vkInvalidateMappedMemoryRanges);
    NEED(vkCreateCommandPool); NEED(vkDestroyCommandPool); NEED(vkAllocateCommandBuffers); NEED(vkBeginCommandBuffer);
    NEED(vkEndCommandBuffer); NEED(vkCmdCopyBuffer); NEED(vkCreateFence); NEED(vkDestroyFence);
    NEED(vkQueueSubmit); NEED(vkWaitForFences); NEED(vkDeviceWaitIdle);
    p_vkGetDeviceQueue(device, queue_family, 0u, &queue);
    if (!queue) { ERR("FAIL: vkGetDeviceQueue returned VK_NULL_HANDLE.\n"); status = 7; goto cleanup; }

    {
        const VkDeviceSize logical_buffer_size = 16ull * MIB;
        VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0u,
            logical_buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_SHARING_MODE_EXCLUSIVE, 0u, NULL };
        VkMemoryRequirements req_a, req_b;
        VkMemoryAllocateInfo allocation;
        int memory_type;
        VkDeviceSize half_block, parent_size;
        uint8_t *bytes;

        VK_CALL("vkCreateBuffer(A: 16MiB transfer src/dst)", p_vkCreateBuffer(device, &buffer_info, NULL, &buffer_a));
        VK_CALL("vkCreateBuffer(B: 16MiB transfer src/dst)", p_vkCreateBuffer(device, &buffer_info, NULL, &buffer_b));
        p_vkGetBufferMemoryRequirements(device, buffer_a, &req_a);
        p_vkGetBufferMemoryRequirements(device, buffer_b, &req_b);
        LOG("REQUIREMENTS A: size=%llu alignment=%llu typeBits=0x%08X\n",
            (unsigned long long)req_a.size, (unsigned long long)req_a.alignment, req_a.memoryTypeBits);
        LOG("REQUIREMENTS B: size=%llu alignment=%llu typeBits=0x%08X\n",
            (unsigned long long)req_b.size, (unsigned long long)req_b.alignment, req_b.memoryTypeBits);
        if (req_a.size != req_b.size || req_a.alignment != req_b.alignment) {
            ERR("FAIL: identical buffers reported inconsistent requirements; no substitute layout is attempted.\n");
            status = 8;
            goto cleanup;
        }
        half_block = align_up_u64(req_a.size, req_a.alignment);
        parent_size = 2u * half_block;
        if (parent_size < 32ull * MIB || parent_size > 64ull * MIB) {
            ERR("FAIL: expected approximately 32/64MiB parent, got %llu bytes from requirements; no altered test is attempted.\n", (unsigned long long)parent_size);
            status = 8;
            goto cleanup;
        }
        memory_type = choose_memory_type(req_a.memoryTypeBits & req_b.memoryTypeBits, &memory_props);
        if (memory_type < 0) {
            ERR("FAIL: no HOST_VISIBLE|HOST_COHERENT memory type compatible with both buffers.\n");
            status = 8;
            goto cleanup;
        }
        LOG("PARENT: size=%llu (%llu MiB), halfBlock=%llu, offsets A=0 B=%llu; memoryType=%d flags=0x%X\n",
            (unsigned long long)parent_size, (unsigned long long)(parent_size / MIB),
            (unsigned long long)half_block, (unsigned long long)half_block, memory_type,
            memory_props.memoryTypes[memory_type].propertyFlags);
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.pNext = NULL;
        allocation.allocationSize = parent_size;
        allocation.memoryTypeIndex = (uint32_t)memory_type;
        VK_CALL("vkAllocateMemory(parent host-visible coherent block)", p_vkAllocateMemory(device, &allocation, NULL, &parent_memory));
        LOG("CALL: vkMapMemory(parent; mapped pattern: first half A, second half B)\n");
        VK_CALL("vkMapMemory(parent)", p_vkMapMemory(device, parent_memory, 0u, parent_size, 0u, &mapped));
        if (!mapped) { ERR("FAIL: vkMapMemory returned success with NULL data.\n"); status = 8; goto cleanup; }
        bytes = (uint8_t *)mapped;
        write_parent_patterns(bytes, half_block);
        LOG("OK: mapped parent patterns written before binds (coherent: no explicit flush required).\n");

        /* The immediately following call is the baseline. It must complete before the offset bind. */
        LOG("CALL: vkBindBufferMemory(A, parent, offset=0) [baseline]\n");
        VK_CALL("vkBindBufferMemory(A, offset=0)", p_vkBindBufferMemory(device, buffer_a, parent_memory, 0u));

        /* Expected fault site on the pre-fix bridge: bindToBuffer -> updateUpload(WholeSize). */
        LOG("CALL: vkBindBufferMemory(B, parent, offset=halfBlock=%llu) [TARGET OFFSET BIND]\n",
            (unsigned long long)half_block);
        VK_CALL("vkBindBufferMemory(B, offset=halfBlock)", p_vkBindBufferMemory(device, buffer_b, parent_memory, half_block));
        LOG("PASS: both raw vkBindBufferMemory calls returned VK_SUCCESS; no bind-time access violation occurred.\n");

        LOG("CALL: vkUnmapMemory(parent) [exercises normal mapped upload propagation]\n");
        p_vkUnmapMemory(device, parent_memory);
        mapped = NULL;
        LOG("RESULT: vkUnmapMemory(parent) => void\n");

        {
            VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queue_family };
            VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
                command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1u };
            VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
            VkBufferCopy copy = { 0u, 0u, logical_buffer_size };
            VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0u };
            VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0u, NULL, NULL, 1u, &command_buffer, 0u, NULL };
            VK_CALL("vkCreateCommandPool(transfer)", p_vkCreateCommandPool(device, &pool_info, NULL, &command_pool));
            command_info.commandPool = command_pool;
            VK_CALL("vkAllocateCommandBuffers", p_vkAllocateCommandBuffers(device, &command_info, &command_buffer));
            VK_CALL("vkBeginCommandBuffer(copy B->A)", p_vkBeginCommandBuffer(command_buffer, &begin_info));
            LOG("CALL: vkCmdCopyBuffer(B offset 0 -> A offset 0, 16MiB)\n");
            p_vkCmdCopyBuffer(command_buffer, buffer_b, buffer_a, 1u, &copy);
            LOG("RESULT: vkCmdCopyBuffer => recorded\n");
            VK_CALL("vkEndCommandBuffer(copy B->A)", p_vkEndCommandBuffer(command_buffer));
            VK_CALL("vkCreateFence(copy)", p_vkCreateFence(device, &fence_info, NULL, &fence));
            VK_CALL("vkQueueSubmit(copy B->A)", p_vkQueueSubmit(queue, 1u, &submit, fence));
            VK_CALL("vkWaitForFences(copy B->A)", p_vkWaitForFences(device, 1u, &fence, VK_TRUE, UINT64_MAX));
            LOG("PASS: transfer copy command submitted and completed.\n");
        }

        LOG("CALL: vkMapMemory(parent readback)\n");
        VK_CALL("vkMapMemory(parent readback)", p_vkMapMemory(device, parent_memory, 0u, parent_size, 0u, &mapped));
        {
            VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, parent_memory, 0u, parent_size };
            VK_CALL("vkInvalidateMappedMemoryRanges(parent readback)", p_vkInvalidateMappedMemoryRanges(device, 1u, &range));
        }
        if (!first_half_is_second_pattern((const uint8_t *)mapped, half_block)) { status = 9; goto cleanup; }
        LOG("PASS: readback samples confirm B's mapped pattern reached logical buffer A through vkCmdCopyBuffer.\n");
        p_vkUnmapMemory(device, parent_memory);
        mapped = NULL;
        LOG("RESULT: vkUnmapMemory(parent readback) => void\n");
    }

    VK_CALL("vkDeviceWaitIdle", p_vkDeviceWaitIdle(device));
    LOG("PASS: focused repro completed. Raw Vulkan reaches vkBindBufferMemory directly; no VMA code is linked or called.\n");
    status = 0;

cleanup:
    if (device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(device);
    if (mapped && parent_memory && p_vkUnmapMemory) p_vkUnmapMemory(device, parent_memory);
    if (fence && p_vkDestroyFence) p_vkDestroyFence(device, fence, NULL);
    if (command_pool && p_vkDestroyCommandPool) p_vkDestroyCommandPool(device, command_pool, NULL);
    if (buffer_b && p_vkDestroyBuffer) p_vkDestroyBuffer(device, buffer_b, NULL);
    if (buffer_a && p_vkDestroyBuffer) p_vkDestroyBuffer(device, buffer_a, NULL);
    if (parent_memory && p_vkFreeMemory) p_vkFreeMemory(device, parent_memory, NULL);
    if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL);
    free(physicals);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    if (status) ERR("FINAL: FAIL status=%d (see last CALL/RESULT above).\n", status);
    return status;
}
