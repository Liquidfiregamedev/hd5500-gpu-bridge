/*
 * Compact raw-Vulkan parent-backing probe for the Ashes D3D11 ICD.
 *
 * Eight simultaneously live 4 KiB VkBuffers are bound at non-zero, 1 MiB
 * strides into one exactly 16 MiB HOST_VISIBLE|HOST_COHERENT VkDeviceMemory.
 * It is deliberately raw Vulkan (no VMA), imports no Vulkan symbols, and
 * loads only the caller-supplied loader after applying process-local ICD
 * environment variables.  The default transfer-only mode has no compute
 * pipeline or dispatch; --storage only adds VK_BUFFER_USAGE_STORAGE_BUFFER_BIT.
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
#include <ctype.h>

#define INTEL_VENDOR_ID 0x8086u
#define KiB (1024ull)
#define MiB (1024ull * 1024ull)
#define CHILD_COUNT 8u
#define CHILD_SIZE (4ull * KiB)
#define STRIDE (1ull * MiB)
#define PARENT_SIZE (16ull * MiB)
#define LAST_CHILD (CHILD_COUNT - 1u)
#define FIRST_CHILD 0u
#define RETIRED_CHILD 1u
#define COPY_SOURCE_OFFSET 13ull
#define COPY_DESTINATION_OFFSET 29ull
#define COPY_BYTES 257ull
#define COPY_TIMEOUT_NS 30000000000ull

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
#define LOAD_INSTANCE(i, n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d, n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)
#define NEED(n) do { \
    if (!p_##n) { ERR("FAIL: required entry point unavailable: %s\n", #n); status = 20; goto cleanup; } \
} while (0)
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

static int instance_extension_present(const char *wanted) {
    uint32_t count = 0, i;
    VkExtensionProperties *list = NULL;
    int found = 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) return 0;
    if (count) {
        list = (VkExtensionProperties *)calloc(count, sizeof(*list));
        if (!list) return 0;
    }
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, list) == VK_SUCCESS) {
        for (i = 0; i < count; ++i)
            if (!strcmp(list[i].extensionName, wanted)) { found = 1; break; }
    }
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
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, list) == VK_SUCCESS) {
        for (i = 0; i < count; ++i)
            if (!strcmp(list[i].extensionName, wanted)) { found = 1; break; }
    }
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

static VkDeviceSize child_offset(uint32_t child) {
    return ((VkDeviceSize)child + 1u) * STRIDE;
}

static uint8_t guard_pattern(VkDeviceSize offset) {
    uint64_t x = (uint64_t)offset;
    return (uint8_t)((x * 0x9E3779B185EBCA87ull) ^ (x >> 19u) ^ 0xC3u);
}

static uint8_t child_pattern(uint32_t child, VkDeviceSize local_offset) {
    uint64_t x = (uint64_t)local_offset;
    return (uint8_t)((0x31u + child * 0x29u) ^ (uint8_t)(x * 0x5Du) ^
                     (uint8_t)(x >> 5u) ^ (uint8_t)(child * 11u));
}

static uint8_t recycled_pattern(VkDeviceSize local_offset) {
    uint64_t x = (uint64_t)local_offset;
    return (uint8_t)(0xE7u ^ (uint8_t)(x * 0x47u) ^ (uint8_t)(x >> 3u));
}

static void initialize_expected(uint8_t *expected) {
    VkDeviceSize i;
    uint32_t child;
    for (i = 0; i < PARENT_SIZE; ++i) expected[i] = guard_pattern(i);
    for (child = 0; child < CHILD_COUNT; ++child) {
        VkDeviceSize base = child_offset(child);
        for (i = 0; i < CHILD_SIZE; ++i) expected[base + i] = child_pattern(child, i);
    }
}

static void set_recycled_expected(uint8_t *expected) {
    VkDeviceSize i, base = child_offset(RETIRED_CHILD);
    for (i = 0; i < CHILD_SIZE; ++i) expected[base + i] = recycled_pattern(i);
}

static void apply_initial_copy_expected(uint8_t *expected) {
    memmove(expected + child_offset(FIRST_CHILD) + COPY_DESTINATION_OFFSET,
            expected + child_offset(LAST_CHILD) + COPY_SOURCE_OFFSET,
            (size_t)COPY_BYTES);
}

static void apply_recycled_copy_expected(uint8_t *expected) {
    memcpy(expected + child_offset(FIRST_CHILD),
           expected + child_offset(RETIRED_CHILD), (size_t)CHILD_SIZE);
}

static const char *offset_region(VkDeviceSize offset) {
    uint32_t child;
    static char label[48];
    for (child = 0; child < CHILD_COUNT; ++child) {
        VkDeviceSize base = child_offset(child);
        if (offset >= base && offset < base + CHILD_SIZE) {
            snprintf(label, sizeof(label), "child[%u] local+%llu", child,
                     (unsigned long long)(offset - base));
            return label;
        }
    }
    return "parent guard/neighbor";
}

static int exact_compare_parent(const uint8_t *actual, const uint8_t *expected,
                                const char *phase) {
    VkDeviceSize i;
    uint64_t mismatches = 0;
    unsigned printed = 0;
    if (!memcmp(actual, expected, (size_t)PARENT_SIZE)) {
        LOG("PASS: %s exact 16 MiB parent comparison passed (destination, all child neighbors, and guard regions).\n", phase);
        return 1;
    }
    for (i = 0; i < PARENT_SIZE; ++i) {
        if (actual[i] != expected[i]) {
            if (printed < 16u) {
                ERR("MISMATCH: %s parent+%llu (%s): actual=0x%02X expected=0x%02X\n",
                    phase, (unsigned long long)i, offset_region(i), actual[i], expected[i]);
                ++printed;
            }
            ++mismatches;
        }
    }
    ERR("FAIL: %s exact 16 MiB parent comparison found %llu mismatched bytes.\n",
        phase, (unsigned long long)mismatches);
    return 0;
}

static VkResult write_full_parent(VkDevice device, VkDeviceMemory memory, const uint8_t *expected) {
    VkResult r;
    void *mapped = NULL;
    VkMappedMemoryRange range;
    r = p_vkMapMemory(device, memory, 0u, PARENT_SIZE, 0u, &mapped);
    if (r != VK_SUCCESS) return r;
    if (!mapped) { p_vkUnmapMemory(device, memory); return VK_ERROR_MEMORY_MAP_FAILED; }
    memcpy(mapped, expected, (size_t)PARENT_SIZE);
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.pNext = NULL;
    range.memory = memory;
    range.offset = 0u;
    range.size = VK_WHOLE_SIZE;
    r = p_vkFlushMappedMemoryRanges(device, 1u, &range);
    p_vkUnmapMemory(device, memory);
    return r;
}

static VkResult readback_exact_parent(VkDevice device, VkDeviceMemory memory,
                                      const uint8_t *expected, const char *phase) {
    VkResult r;
    void *mapped = NULL;
    VkMappedMemoryRange range;
    int ok;
    r = p_vkMapMemory(device, memory, 0u, PARENT_SIZE, 0u, &mapped);
    if (r != VK_SUCCESS) return r;
    if (!mapped) { p_vkUnmapMemory(device, memory); return VK_ERROR_MEMORY_MAP_FAILED; }
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.pNext = NULL;
    range.memory = memory;
    range.offset = 0u;
    range.size = VK_WHOLE_SIZE;
    r = p_vkInvalidateMappedMemoryRanges(device, 1u, &range);
    if (r != VK_SUCCESS) { p_vkUnmapMemory(device, memory); return r; }
    ok = exact_compare_parent((const uint8_t *)mapped, expected, phase);
    p_vkUnmapMemory(device, memory);
    return ok ? VK_SUCCESS : VK_ERROR_UNKNOWN;
}

static VkResult submit_one_copy(VkDevice device, VkQueue queue, uint32_t queue_family,
                                VkBuffer source, VkDeviceSize source_offset,
                                VkBuffer destination, VkDeviceSize destination_offset,
                                VkDeviceSize bytes, const char *label) {
    VkResult r = VK_SUCCESS;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queue_family };
    VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1u };
    VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0u };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0u, NULL, NULL, 1u,
        &command, 0u, NULL };
    VkBufferCopy copy = { source_offset, destination_offset, bytes };

    r = p_vkCreateCommandPool(device, &pool_info, NULL, &pool);
    if (r != VK_SUCCESS) goto done;
    command_info.commandPool = pool;
    r = p_vkAllocateCommandBuffers(device, &command_info, &command);
    if (r != VK_SUCCESS) goto done;
    r = p_vkBeginCommandBuffer(command, &begin_info);
    if (r != VK_SUCCESS) goto done;
    LOG("CALL: vkCmdCopyBuffer(%s: src+%llu -> dst+%llu, %llu bytes)\n", label,
        (unsigned long long)source_offset, (unsigned long long)destination_offset,
        (unsigned long long)bytes);
    p_vkCmdCopyBuffer(command, source, destination, 1u, &copy);
    r = p_vkEndCommandBuffer(command);
    if (r != VK_SUCCESS) goto done;
    r = p_vkCreateFence(device, &fence_info, NULL, &fence);
    if (r != VK_SUCCESS) goto done;
    r = p_vkQueueSubmit(queue, 1u, &submit, fence);
    if (r != VK_SUCCESS) goto done;
    r = p_vkWaitForFences(device, 1u, &fence, VK_TRUE, COPY_TIMEOUT_NS);

done:
    if (fence) p_vkDestroyFence(device, fence, NULL);
    if (pool) p_vkDestroyCommandPool(device, pool, NULL);
    return r;
}

static int requirements_fit_parent(const VkMemoryRequirements *req, uint32_t child,
                                   const char *what) {
    VkDeviceSize offset = child_offset(child);
    if (req->size > STRIDE || req->alignment == 0u || offset % req->alignment != 0u ||
        offset > PARENT_SIZE || req->size > PARENT_SIZE - offset) {
        ERR("FAIL: %s requirements do not fit designated parent slot: size=%llu alignment=%llu offset=%llu parent=%llu stride=%llu.\n",
            what, (unsigned long long)req->size, (unsigned long long)req->alignment,
            (unsigned long long)offset, (unsigned long long)PARENT_SIZE,
            (unsigned long long)STRIDE);
        return 0;
    }
    return 1;
}

static void usage(const wchar_t *exe) {
    LOG("Usage:\n  %ls --loader C:\\\\absolute\\\\vulkan-1.dll --icd C:\\\\absolute\\\\ashesD3D11Renderer_icd.json [--storage] [--allow-non-intel-test]\n", exe);
    LOG("Default mode: TRANSFER_SRC|TRANSFER_DST only. --storage adds STORAGE_BUFFER but creates no compute pipeline or dispatch.\n");
    LOG("--loader and --icd must be absolute Windows paths. The Intel/non-software adapter gate is default; --allow-non-intel-test is sandbox-only control.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL;
    int allow_non_intel = 0;
    int storage_mode = 0;
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
    VkBuffer children[CHILD_COUNT];
    VkBuffer recycled_child = VK_NULL_HANDLE;
    VkBuffer unused_storage_buffer = VK_NULL_HANDLE;
    VkDeviceMemory parent_memory = VK_NULL_HANDLE;
    uint8_t *expected = NULL;
    uint32_t common_type_bits = 0xFFFFFFFFu;
    VkBufferUsageFlags buffer_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } loader_gipa;

    memset(&selected_props, 0, sizeof(selected_props));
    memset(&memory_props, 0, sizeof(memory_props));
    memset(children, 0, sizeof(children));

    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--storage")) storage_mode = 1;
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

    LOG("Compact parent-backing buffer-bind probe (raw Vulkan; no VMA)\n");
    LOG("CONTRACT: 8 simultaneously live 4 KiB buffers, offsets 1..8 MiB, one exactly 16 MiB parent allocation.\n");
    LOG("LOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    LOG("ICD isolation: VK_ICD_FILENAMES and VK_DRIVER_FILES are set in this process only.\n");
    if (storage_mode) {
        buffer_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        LOG("MODE: --storage: children include TRANSFER_SRC|TRANSFER_DST|STORAGE_BUFFER; no compute work is created.\n");
    } else {
        LOG("MODE: default transfer-only: children include TRANSFER_SRC|TRANSFER_DST; no compute work is created.\n");
    }
    if (allow_non_intel)
        LOG("WARNING: --allow-non-intel-test accepts a Wine/synthetic/software-backed adapter only for sandbox integration; it is NOT Intel hardware validation.\n");

    loader = LoadLibraryW(loader_path);
    if (!loader) { ERR("FAIL: LoadLibraryW(loader) error %lu\n", GetLastError()); status = 4; goto cleanup; }
    loader_gipa.raw = GetProcAddress(loader, "vkGetInstanceProcAddr");
    gipa = loader_gipa.typed;
    if (!gipa) { ERR("FAIL: loader exposes no vkGetInstanceProcAddr.\n"); status = 4; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion);
    LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties);
    LOAD_GLOBAL(vkCreateInstance);
    NEED(vkEnumerateInstanceExtensionProperties); NEED(vkCreateInstance);
    if (p_vkEnumerateInstanceVersion) {
        uint32_t version = VK_API_VERSION_1_0;
        VK_CALL("vkEnumerateInstanceVersion", p_vkEnumerateInstanceVersion(&version));
        LOG("LOADER MAX API: %u.%u.%u; application requests 1.0.0.\n",
            VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    }
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL,
            "Compact parent backing buffer-bind probe", 1u, "standalone", 1u, VK_API_VERSION_1_0 };
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
        LOG("WARNING: selected adapter is synthetic/non-Intel and/or software-like: sandbox integration result only, NOT Intel hardware validation.\n");
    LOG("SELECTED: %s, vendor=0x%04X, queueFamily=%u\n", selected_props.deviceName,
        selected_props.vendorID, queue_family);
    p_vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);

    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL,
            0u, queue_family, 1u, &priority };
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
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0u,
            CHILD_SIZE, buffer_usage, VK_SHARING_MODE_EXCLUSIVE, 0u, NULL };
        VkMemoryAllocateInfo allocation;
        int memory_type;
        uint32_t child;

        for (child = 0; child < CHILD_COUNT; ++child) {
            VkMemoryRequirements req;
            char label[96];
            snprintf(label, sizeof(label), "vkCreateBuffer(child[%u], 4KiB)", child);
            VK_CALL(label, p_vkCreateBuffer(device, &info, NULL, &children[child]));
            p_vkGetBufferMemoryRequirements(device, children[child], &req);
            LOG("REQUIREMENTS child[%u]: size=%llu alignment=%llu typeBits=0x%08X; target offset=%llu\n",
                child, (unsigned long long)req.size, (unsigned long long)req.alignment,
                req.memoryTypeBits, (unsigned long long)child_offset(child));
            if (!requirements_fit_parent(&req, child, "child")) { status = 8; goto cleanup; }
            common_type_bits &= req.memoryTypeBits;
        }
        memory_type = choose_memory_type(common_type_bits, &memory_props);
        if (memory_type < 0) {
            ERR("FAIL: no common HOST_VISIBLE|HOST_COHERENT memory type for all eight live children. commonTypeBits=0x%08X\n",
                common_type_bits);
            status = 8;
            goto cleanup;
        }
        LOG("PARENT: allocationSize=%llu (%llu MiB) exactly; memoryType=%d flags=0x%X; all child offsets are non-zero 1MiB strides.\n",
            (unsigned long long)PARENT_SIZE, (unsigned long long)(PARENT_SIZE / MiB), memory_type,
            memory_props.memoryTypes[memory_type].propertyFlags);
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.pNext = NULL;
        allocation.allocationSize = PARENT_SIZE;
        allocation.memoryTypeIndex = (uint32_t)memory_type;
        VK_CALL("vkAllocateMemory(exact 16MiB parent)", p_vkAllocateMemory(device, &allocation, NULL, &parent_memory));

        for (child = 0; child < CHILD_COUNT; ++child) {
            char label[120];
            snprintf(label, sizeof(label), "vkBindBufferMemory(child[%u], parent, nonzero offset=%llu)",
                     child, (unsigned long long)child_offset(child));
            VK_CALL(label, p_vkBindBufferMemory(device, children[child], parent_memory, child_offset(child)));
        }
        LOG("PASS: all eight buffers are simultaneously live and bound to the single parent allocation.\n");

        if (storage_mode) {
            VkBufferCreateInfo unused_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0u,
                256u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0u, NULL };
            VK_CALL("vkCreateBuffer(unused unbound storage cleanup sentinel)",
                    p_vkCreateBuffer(device, &unused_info, NULL, &unused_storage_buffer));
            LOG("LIFETIME: unused storage sentinel is deliberately unbound; cleanup frees the parent first only after all parent-bound children are destroyed.\n");
        }

        expected = (uint8_t *)malloc((size_t)PARENT_SIZE);
        if (!expected) { ERR("FAIL: host allocation for exact parent expected image.\n"); status = 8; goto cleanup; }
        initialize_expected(expected);
        LOG("CALL: vkMapMemory(parent); write unique child patterns plus guard patterns; vkFlushMappedMemoryRanges; vkUnmapMemory.\n");
        VK_CALL("map/write/flush/unmap initial exact parent image", write_full_parent(device, parent_memory, expected));
        LOG("PASS: parent mapping was explicitly flushed and unmapped before queue work.\n");

        VK_CALL("initial subset copy last->first (source+13, destination+29, 257 bytes)",
                submit_one_copy(device, queue, queue_family, children[LAST_CHILD], COPY_SOURCE_OFFSET,
                                children[FIRST_CHILD], COPY_DESTINATION_OFFSET, COPY_BYTES,
                                "last child -> first child subset"));
        VK_CALL("vkDeviceWaitIdle after initial subset copy", p_vkDeviceWaitIdle(device));
        apply_initial_copy_expected(expected);
        LOG("CALL: vkMapMemory(parent readback); vkInvalidateMappedMemoryRanges; byte-exact compare; vkUnmapMemory.\n");
        {
            VkResult verify = readback_exact_parent(device, parent_memory, expected,
                "INITIAL READBACK (257-byte destination and unchanged neighbors)");
            LOG("RESULT: initial map/invalidate/exact-readback => %s (%d)\n", result_name(verify), (int)verify);
            if (verify != VK_SUCCESS) { status = 9; goto cleanup; }
        }

        LOG("LIFETIME: destroying child[%u] while retaining the live parent allocation, then reusing its exact non-zero offset %llu.\n",
            RETIRED_CHILD, (unsigned long long)child_offset(RETIRED_CHILD));
        p_vkDestroyBuffer(device, children[RETIRED_CHILD], NULL);
        children[RETIRED_CHILD] = VK_NULL_HANDLE;
        VK_CALL("vkCreateBuffer(recycled child at released slot)", p_vkCreateBuffer(device, &info, NULL, &recycled_child));
        {
            VkMemoryRequirements recycled_req;
            p_vkGetBufferMemoryRequirements(device, recycled_child, &recycled_req);
            LOG("REQUIREMENTS recycled child: size=%llu alignment=%llu typeBits=0x%08X; reused offset=%llu\n",
                (unsigned long long)recycled_req.size, (unsigned long long)recycled_req.alignment,
                recycled_req.memoryTypeBits, (unsigned long long)child_offset(RETIRED_CHILD));
            if (!requirements_fit_parent(&recycled_req, RETIRED_CHILD, "recycled child") ||
                !(recycled_req.memoryTypeBits & (1u << (uint32_t)memory_type))) {
                ERR("FAIL: recycled child is not compatible with the retained parent allocation/type.\n");
                status = 10;
                goto cleanup;
            }
        }
        VK_CALL("vkBindBufferMemory(recycled child, retained parent, released offset)",
                p_vkBindBufferMemory(device, recycled_child, parent_memory, child_offset(RETIRED_CHILD)));
        set_recycled_expected(expected);
        LOG("CALL: vkMapMemory(retained parent); write recycled-slot unique pattern; vkFlushMappedMemoryRanges; vkUnmapMemory.\n");
        VK_CALL("map/write/flush/unmap recycled-slot pattern", write_full_parent(device, parent_memory, expected));
        VK_CALL("recycled-slot full copy -> first child", submit_one_copy(device, queue, queue_family,
                recycled_child, 0u, children[FIRST_CHILD], 0u, CHILD_SIZE,
                "recycled slot -> first child full proof"));
        VK_CALL("vkDeviceWaitIdle after recycled-slot proof", p_vkDeviceWaitIdle(device));
        apply_recycled_copy_expected(expected);
        {
            VkResult verify = readback_exact_parent(device, parent_memory, expected,
                "RECYCLED-SLOT READBACK (released child, retained parent, reused offset)");
            LOG("RESULT: recycled-slot map/invalidate/exact-readback => %s (%d)\n", result_name(verify), (int)verify);
            if (verify != VK_SUCCESS) { status = 11; goto cleanup; }
        }
    }

    LOG("PASS: compact parent-backing fixture completed: bind, subset-copy integrity, neighbor integrity, and release/reuse lifecycle all passed.\n");
    status = 0;

cleanup:
    if (device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(device);
    if (recycled_child && p_vkDestroyBuffer) {
        LOG("CLEANUP: vkDestroyBuffer(recycled child) before releasing parent memory.\n");
        p_vkDestroyBuffer(device, recycled_child, NULL);
    }
    for (i = (int)CHILD_COUNT - 1; i >= 0; --i) {
        if (children[i] && p_vkDestroyBuffer) {
            LOG("CLEANUP: vkDestroyBuffer(child[%d]) before releasing parent memory.\n", i);
            p_vkDestroyBuffer(device, children[i], NULL);
        }
    }
    if (parent_memory && p_vkFreeMemory) {
        LOG("CLEANUP: vkFreeMemory(parent) after every parent-bound child was destroyed.\n");
        p_vkFreeMemory(device, parent_memory, NULL);
    }
    if (unused_storage_buffer && p_vkDestroyBuffer) {
        LOG("CLEANUP: vkDestroyBuffer(unused unbound storage sentinel) after parent free.\n");
        p_vkDestroyBuffer(device, unused_storage_buffer, NULL);
    }
    free(expected);
    if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL);
    free(physicals);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    if (status) ERR("FINAL: FAIL status=%d (see last CALL/RESULT above).\n", status);
    return status;
}
