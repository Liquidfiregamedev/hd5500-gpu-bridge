/*
 * Vulkan 1.0 GPU ICD validation probe for the patched Ashes D3D11 ICD.
 *
 * Deliberately links no Vulkan import library.  It loads only the caller-supplied,
 * absolute stock vulkan-1.dll path after setting VK_ICD_FILENAMES in *this process*.
 * No registry, system environment, driver, or game files are modified.
 *
 * Uses Vulkan 1.0 entry points and structures only.  The compute shader is embedded
 * from a separately retained, spirv-val validated SPIR-V 1.0 binary.
 */
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "compute_5500_spv.h"

#define INTEL_VENDOR_ID 0x8086u
#define MICROSOFT_VENDOR_ID 0x1414u

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
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties;
static PFN_vkGetPhysicalDeviceSurfaceSupportKHR p_vkGetPhysicalDeviceSurfaceSupportKHR;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
static PFN_vkGetPhysicalDeviceSurfaceFormatsKHR p_vkGetPhysicalDeviceSurfaceFormatsKHR;
static PFN_vkGetPhysicalDeviceSurfacePresentModesKHR p_vkGetPhysicalDeviceSurfacePresentModesKHR;
static PFN_vkCreateWin32SurfaceKHR p_vkCreateWin32SurfaceKHR;
static PFN_vkDestroySurfaceKHR p_vkDestroySurfaceKHR;
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
static PFN_vkCreateDescriptorSetLayout p_vkCreateDescriptorSetLayout;
static PFN_vkDestroyDescriptorSetLayout p_vkDestroyDescriptorSetLayout;
static PFN_vkCreateDescriptorPool p_vkCreateDescriptorPool;
static PFN_vkDestroyDescriptorPool p_vkDestroyDescriptorPool;
static PFN_vkAllocateDescriptorSets p_vkAllocateDescriptorSets;
static PFN_vkUpdateDescriptorSets p_vkUpdateDescriptorSets;
static PFN_vkCreatePipelineLayout p_vkCreatePipelineLayout;
static PFN_vkDestroyPipelineLayout p_vkDestroyPipelineLayout;
static PFN_vkCreateShaderModule p_vkCreateShaderModule;
static PFN_vkDestroyShaderModule p_vkDestroyShaderModule;
static PFN_vkCreateComputePipelines p_vkCreateComputePipelines;
static PFN_vkDestroyPipeline p_vkDestroyPipeline;
static PFN_vkCreateCommandPool p_vkCreateCommandPool;
static PFN_vkDestroyCommandPool p_vkDestroyCommandPool;
static PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
static PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
static PFN_vkCmdBindPipeline p_vkCmdBindPipeline;
static PFN_vkCmdBindDescriptorSets p_vkCmdBindDescriptorSets;
static PFN_vkCmdDispatch p_vkCmdDispatch;
static PFN_vkCmdClearColorImage p_vkCmdClearColorImage;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkDestroyFence p_vkDestroyFence;
static PFN_vkWaitForFences p_vkWaitForFences;
static PFN_vkCreateSemaphore p_vkCreateSemaphore;
static PFN_vkDestroySemaphore p_vkDestroySemaphore;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle;
static PFN_vkCreateSwapchainKHR p_vkCreateSwapchainKHR;
static PFN_vkDestroySwapchainKHR p_vkDestroySwapchainKHR;
static PFN_vkGetSwapchainImagesKHR p_vkGetSwapchainImagesKHR;
static PFN_vkAcquireNextImageKHR p_vkAcquireNextImageKHR;
static PFN_vkQueuePresentKHR p_vkQueuePresentKHR;

#define LOAD_GLOBAL(n) p_##n = (PFN_##n)gipa(VK_NULL_HANDLE, #n)
#define LOAD_INSTANCE(i,n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d,n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)
#define NEED(n) do { if (!(p_##n)) { fprintf(stderr, "FAIL: required Vulkan entry point is unavailable: %s\n", #n); status = 20; goto cleanup; } } while (0)
#define VK_TRY(label, expr) do { fprintf(stderr, "CALL: %s\n", (label)); fflush(stderr); VkResult vr_ = (expr); if (vr_ != VK_SUCCESS) { fprintf(stderr, "FAIL: %s returned %s (%d)\n", (label), result_name(vr_), (int)vr_); status = 21; goto cleanup; } fprintf(stderr, "OK: %s\n", (label)); fflush(stderr); } while (0)

static const char *result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "unrecognized VkResult";
    }
}

static void print_version(const char *label, uint32_t v) {
    printf("%s%u.%u.%u", label, VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
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

static int is_software_or_warp(const VkPhysicalDeviceProperties *p) {
    return p->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
        p->vendorID == MICROSOFT_VENDOR_ID ||
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

static void print_uuid(const uint8_t *uuid) {
    uint32_t i;
    for (i = 0; i < VK_UUID_SIZE; ++i) printf("%02X", uuid[i]);
}

static void print_memory_flags(VkMemoryPropertyFlags f) {
    if (!f) { printf("0"); return; }
    if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) printf(" DEVICE_LOCAL");
    if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) printf(" HOST_VISIBLE");
    if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) printf(" HOST_COHERENT");
    if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) printf(" HOST_CACHED");
    if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) printf(" LAZILY_ALLOCATED");
}

static void print_features(const VkPhysicalDeviceFeatures *f) {
#define F(x) printf("  %-40s %s\n", #x, f->x ? "VK_TRUE" : "VK_FALSE")
    printf("Selected-device VkPhysicalDeviceFeatures (actual query):\n");
    F(robustBufferAccess); F(fullDrawIndexUint32); F(imageCubeArray); F(independentBlend);
    F(geometryShader); F(tessellationShader); F(sampleRateShading); F(dualSrcBlend);
    F(logicOp); F(multiDrawIndirect); F(drawIndirectFirstInstance); F(depthClamp);
    F(depthBiasClamp); F(fillModeNonSolid); F(depthBounds); F(wideLines);
    F(largePoints); F(alphaToOne); F(multiViewport); F(samplerAnisotropy);
    F(textureCompressionETC2); F(textureCompressionASTC_LDR); F(textureCompressionBC);
    F(occlusionQueryPrecise); F(pipelineStatisticsQuery); F(vertexPipelineStoresAndAtomics);
    F(fragmentStoresAndAtomics); F(shaderTessellationAndGeometryPointSize); F(shaderImageGatherExtended);
    F(shaderStorageImageExtendedFormats); F(shaderStorageImageMultisample); F(shaderStorageImageReadWithoutFormat);
    F(shaderStorageImageWriteWithoutFormat); F(shaderUniformBufferArrayDynamicIndexing);
    F(shaderSampledImageArrayDynamicIndexing); F(shaderStorageBufferArrayDynamicIndexing);
    F(shaderStorageImageArrayDynamicIndexing); F(shaderClipDistance); F(shaderCullDistance);
    F(shaderFloat64); F(shaderInt64); F(shaderInt16); F(shaderResourceResidency);
    F(shaderResourceMinLod); F(sparseBinding); F(sparseResidencyBuffer); F(sparseResidencyImage2D);
    F(sparseResidencyImage3D); F(sparseResidency2Samples); F(sparseResidency4Samples);
    F(sparseResidency8Samples); F(sparseResidency16Samples); F(sparseResidencyAliased);
    F(variableMultisampleRate); F(inheritedQueries);
#undef F
}

static void print_device_report(VkPhysicalDevice physical) {
    VkPhysicalDeviceProperties p;
    VkPhysicalDeviceFeatures f;
    VkPhysicalDeviceMemoryProperties m;
    uint32_t i;
    p_vkGetPhysicalDeviceProperties(physical, &p);
    p_vkGetPhysicalDeviceFeatures(physical, &f);
    p_vkGetPhysicalDeviceMemoryProperties(physical, &m);
    printf("\n=== SELECTED PHYSICAL DEVICE: actual ICD report ===\n");
    printf("deviceName=%s\n", p.deviceName);
    printf("vendorID=0x%04X deviceID=0x%04X deviceType=%s (%u)\n",
           p.vendorID, p.deviceID, device_type_name(p.deviceType), (unsigned)p.deviceType);
    print_version("apiVersion=", p.apiVersion); printf("\n");
    printf("driverVersion=0x%08X (raw vendor-defined encoding)\n", p.driverVersion);
    printf("pipelineCacheUUID="); print_uuid(p.pipelineCacheUUID); printf("\n");
    printf("limits: maxComputeSharedMemorySize=%u maxComputeWorkGroupInvocations=%u "
           "maxComputeWorkGroupCount=[%u,%u,%u] maxStorageBufferRange=%u "
           "nonCoherentAtomSize=%llu\n",
           p.limits.maxComputeSharedMemorySize, p.limits.maxComputeWorkGroupInvocations,
           p.limits.maxComputeWorkGroupCount[0], p.limits.maxComputeWorkGroupCount[1],
           p.limits.maxComputeWorkGroupCount[2], p.limits.maxStorageBufferRange,
           (unsigned long long)p.limits.nonCoherentAtomSize);
    print_features(&f);
    printf("Memory heaps (actual query): count=%u\n", m.memoryHeapCount);
    for (i = 0; i < m.memoryHeapCount; ++i)
        printf("  heap[%u]: size=%llu flags=0x%X%s\n", i,
               (unsigned long long)m.memoryHeaps[i].size, m.memoryHeaps[i].flags,
               (m.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "");
    printf("Memory types (actual query): count=%u\n", m.memoryTypeCount);
    for (i = 0; i < m.memoryTypeCount; ++i) {
        printf("  type[%u]: heapIndex=%u flags=0x%X", i, m.memoryTypes[i].heapIndex,
               m.memoryTypes[i].propertyFlags);
        print_memory_flags(m.memoryTypes[i].propertyFlags);
        printf("\n");
    }
}

static int print_instance_extensions(void) {
    uint32_t count = 0, i;
    VkExtensionProperties *list;
    VkResult r = p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    if (r != VK_SUCCESS) { fprintf(stderr, "FAIL: vkEnumerateInstanceExtensionProperties(count) returned %s (%d)\n", result_name(r), (int)r); return 0; }
    list = count ? (VkExtensionProperties *)calloc(count, sizeof(*list)) : NULL;
    if (count && !list) { fprintf(stderr, "FAIL: out of host memory allocating instance-extension list\n"); return 0; }
    r = p_vkEnumerateInstanceExtensionProperties(NULL, &count, list);
    if (r != VK_SUCCESS) { fprintf(stderr, "FAIL: vkEnumerateInstanceExtensionProperties(list) returned %s (%d)\n", result_name(r), (int)r); free(list); return 0; }
    printf("Instance extensions reported by supplied loader/selected ICD (%u):\n", count);
    for (i = 0; i < count; ++i) printf("  %s (spec %u)\n", list[i].extensionName, list[i].specVersion);
    free(list);
    return 1;
}

static int instance_extension_present(const char *wanted) {
    uint32_t count = 0, i;
    VkExtensionProperties *list;
    int found = 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) return 0;
    list = count ? (VkExtensionProperties *)calloc(count, sizeof(*list)) : NULL;
    if (count && !list) return 0;
    if (p_vkEnumerateInstanceExtensionProperties(NULL, &count, list) == VK_SUCCESS)
        for (i = 0; i < count; ++i) if (!strcmp(list[i].extensionName, wanted)) { found = 1; break; }
    free(list);
    return found;
}

static int device_extension_present(VkPhysicalDevice physical, const char *wanted, int print_all) {
    uint32_t count = 0, i;
    VkExtensionProperties *list;
    VkResult r = p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL);
    int found = 0;
    if (r != VK_SUCCESS) { fprintf(stderr, "FAIL: vkEnumerateDeviceExtensionProperties(count) returned %s (%d)\n", result_name(r), (int)r); return 0; }
    list = count ? (VkExtensionProperties *)calloc(count, sizeof(*list)) : NULL;
    if (count && !list) { fprintf(stderr, "FAIL: out of host memory allocating device-extension list\n"); return 0; }
    r = p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, list);
    if (r != VK_SUCCESS) { fprintf(stderr, "FAIL: vkEnumerateDeviceExtensionProperties(list) returned %s (%d)\n", result_name(r), (int)r); free(list); return 0; }
    if (print_all) printf("Device extensions reported by selected physical device (%u):\n", count);
    for (i = 0; i < count; ++i) {
        if (print_all) printf("  %s (spec %u)\n", list[i].extensionName, list[i].specVersion);
        if (!strcmp(list[i].extensionName, wanted)) found = 1;
    }
    free(list);
    return found;
}

static int find_unified_queue(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *out_family) {
    uint32_t count = 0, i;
    VkQueueFamilyProperties *q;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    q = count ? (VkQueueFamilyProperties *)calloc(count, sizeof(*q)) : NULL;
    if (!count || !q) { fprintf(stderr, "FAIL: no host memory or no queue families on candidate device\n"); return 0; }
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, q);
    for (i = 0; i < count; ++i) {
        VkBool32 present = VK_FALSE;
        VkResult r = p_vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
        printf("  queueFamily[%u]: flags=0x%X queueCount=%u surfaceSupport=%s", i,
               q[i].queueFlags, q[i].queueCount, r == VK_SUCCESS ? (present ? "VK_TRUE" : "VK_FALSE") : result_name(r));
        printf("\n");
        if (r == VK_SUCCESS && q[i].queueCount &&
            (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            *out_family = i; free(q); return 1;
        }
    }
    free(q);
    return 0;
}

static int choose_memory_type(uint32_t bits, VkMemoryPropertyFlags required,
                              const VkPhysicalDeviceMemoryProperties *m) {
    uint32_t i;
    for (i = 0; i < m->memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m->memoryTypes[i].propertyFlags & required) == required) return (int)i;
    return -1;
}

static void pump_messages(DWORD milliseconds) {
    DWORD end = GetTickCount() + milliseconds;
    MSG msg;
    do {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(10);
    } while ((LONG)(GetTickCount() - end) < 0);
}

static void usage(const wchar_t *exe) {
    printf("Vulkan 1.0 GPU ICD validation probe (Windows x64)\n\n");
    printf("Usage:\n  %ls --loader C:\\\\absolute\\\\path\\\\vulkan-1.dll --icd C:\\\\absolute\\\\path\\\\icd-manifest.json [--allow-non-intel-test]\n\n", exe);
    printf("--loader and --icd are mandatory absolute existing paths.  The probe sets VK_ICD_FILENAMES\n");
    printf("only for its own process before loading the supplied stock loader.  Default policy requires\n");
    printf("a non-software Intel (vendor 0x8086) GPU.  --allow-non-intel-test is only for an explicit\n");
    printf("sandbox/Wine virtual-adapter experiment and is loudly marked non-Intel; CPU/WARP/software remain rejected.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL;
    int allow_non_intel = 0, status = 1, i;
    int no_portability_optin = 0;
    HMODULE loader = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice *physical_list = NULL, selected = VK_NULL_HANDLE;
    uint32_t physical_count = 0, family = UINT32_MAX;
    HWND window = NULL;
    ATOM class_atom = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkBuffer result_buffer = VK_NULL_HANDLE;
    VkDeviceMemory result_memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader_module = VK_NULL_HANDLE;
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkFence compute_fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t loader_version = VK_API_VERSION_1_0;
    VkPhysicalDeviceProperties selected_props;
    VkPhysicalDeviceMemoryProperties selected_memory;
    int selected_memory_type = -1, coherent = 0;
    union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } loader_gipa;

    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { usage(argv[0]); return 0; }
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--no-portability-optin")) no_portability_optin = 1;
        else { fprintf(stderr, "FAIL: unrecognized or incomplete argument: %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!loader_path || !icd_path) { fprintf(stderr, "FAIL: --loader and --icd are both required.\n"); usage(argv[0]); return 2; }
    if (!is_absolute_windows_path(loader_path) || !is_absolute_windows_path(icd_path)) {
        fprintf(stderr, "FAIL: --loader and --icd must be absolute Windows paths; no default loader or ICD is accepted.\n"); return 2;
    }
    if (GetFileAttributesW(loader_path) == INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "FAIL: supplied --loader path does not exist (Win32 error %lu): %ls\n", GetLastError(), loader_path); return 2; }
    if (GetFileAttributesW(icd_path) == INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "FAIL: supplied --icd path does not exist (Win32 error %lu): %ls\n", GetLastError(), icd_path); return 2; }
    if (GetModuleHandleW(L"vulkan-1.dll")) {
        fprintf(stderr, "FAIL: vulkan-1.dll was already loaded before ICD isolation; refusing an ambiguous loader/ICD selection.\n"); return 3;
    }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path)) { fprintf(stderr, "FAIL: SetEnvironmentVariableW(VK_ICD_FILENAMES) failed (Win32 error %lu)\n", GetLastError()); return 3; }
    /* VK_DRIVER_FILES is the current loader spelling; the older loader ignores it. */
    if (!SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) { fprintf(stderr, "FAIL: SetEnvironmentVariableW(VK_DRIVER_FILES) failed (Win32 error %lu)\n", GetLastError()); return 3; }

    printf("Vulkan 1.0 GPU ICD validation probe\n");
    printf("ICD selection: VK_ICD_FILENAMES and VK_DRIVER_FILES set in this process only to %ls\n", icd_path);
    printf("Loader selection: supplied absolute path %ls\n", loader_path);
    printf("Requested Vulkan application API: 1.0.0 exactly (no 1.1+ features or calls).\n");
    if (allow_non_intel) printf("WARNING: --allow-non-intel-test supplied. Any selected non-Intel adapter is NOT an Intel HD 5500 hardware validation.\n");

    loader = LoadLibraryW(loader_path);
    if (!loader) { fprintf(stderr, "FAIL: LoadLibraryW(supplied --loader) failed (Win32 error %lu)\n", GetLastError()); status = 4; goto cleanup; }
    loader_gipa.raw = GetProcAddress(loader, "vkGetInstanceProcAddr");
    gipa = loader_gipa.typed;
    if (!gipa) { fprintf(stderr, "FAIL: supplied loader has no vkGetInstanceProcAddr export.\n"); status = 4; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion);
    if (p_vkEnumerateInstanceVersion) {
        VK_TRY("vkEnumerateInstanceVersion", p_vkEnumerateInstanceVersion(&loader_version));
    } else {
        printf("Loader does not export vkEnumerateInstanceVersion: treating it as Vulkan 1.0 per loader compatibility rules.\n");
    }
    print_version("Loader maximum instance API reported: ", loader_version); printf("\n");
    LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties); LOAD_GLOBAL(vkCreateInstance);
    NEED(vkEnumerateInstanceExtensionProperties); NEED(vkCreateInstance);
    if (!print_instance_extensions()) { status = 5; goto cleanup; }
    if (!instance_extension_present(VK_KHR_SURFACE_EXTENSION_NAME) || !instance_extension_present(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
        fprintf(stderr, "FAIL: selected loader/ICD does not report both VK_KHR_surface and VK_KHR_win32_surface.\n"); status = 5; goto cleanup;
    }
    {
        const char *instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME };
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Ashes D3D11 ICD Vulkan 1.0 probe", 1, "standalone validation", 1, VK_API_VERSION_1_0 };
        VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, instance_extensions };
        if (!no_portability_optin && instance_extension_present(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            ci.enabledExtensionCount = 3;
            ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            printf("Portability: explicitly enabling the advertised VK_KHR_portability_enumeration extension and enumeration flag; API remains 1.0.\n");
        }
        VK_TRY("vkCreateInstance(apiVersion=VK_API_VERSION_1_0)", p_vkCreateInstance(&ci, NULL, &instance));
    }
    printf("PASS: vkCreateInstance requested and created exactly Vulkan 1.0.\n");
    LOAD_INSTANCE(instance, vkDestroyInstance); LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceFeatures);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceSurfaceCapabilitiesKHR); LOAD_INSTANCE(instance, vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceSurfacePresentModesKHR); LOAD_INSTANCE(instance, vkCreateWin32SurfaceKHR);
    LOAD_INSTANCE(instance, vkDestroySurfaceKHR); LOAD_INSTANCE(instance, vkCreateDevice); LOAD_INSTANCE(instance, vkGetDeviceProcAddr);
    NEED(vkDestroyInstance); NEED(vkEnumeratePhysicalDevices); NEED(vkGetPhysicalDeviceProperties); NEED(vkGetPhysicalDeviceFeatures);
    NEED(vkGetPhysicalDeviceMemoryProperties); NEED(vkGetPhysicalDeviceQueueFamilyProperties); NEED(vkEnumerateDeviceExtensionProperties);
    NEED(vkGetPhysicalDeviceSurfaceSupportKHR); NEED(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); NEED(vkGetPhysicalDeviceSurfaceFormatsKHR);
    NEED(vkGetPhysicalDeviceSurfacePresentModesKHR); NEED(vkCreateWin32SurfaceKHR); NEED(vkDestroySurfaceKHR); NEED(vkCreateDevice); NEED(vkGetDeviceProcAddr);

    {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"AshesVulkan10GpuIcdProbeWindow";
        class_atom = RegisterClassW(&wc);
        if (!class_atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { fprintf(stderr, "FAIL: RegisterClassW failed (Win32 error %lu)\n", GetLastError()); status = 6; goto cleanup; }
        window = CreateWindowExW(0, wc.lpszClassName, L"Vulkan 1.0 ICD probe: cleared frame", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 360, 280, NULL, NULL, wc.hInstance, NULL);
        if (!window) { fprintf(stderr, "FAIL: CreateWindowExW failed (Win32 error %lu)\n", GetLastError()); status = 6; goto cleanup; }
        ShowWindow(window, SW_SHOW); UpdateWindow(window);
        {
            VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, wc.hInstance, window };
            VK_TRY("vkCreateWin32SurfaceKHR", p_vkCreateWin32SurfaceKHR(instance, &sci, NULL, &surface));
        }
        printf("PASS: created visible Win32 test window and VkSurfaceKHR.\n");
    }

    VK_TRY("vkEnumeratePhysicalDevices(count)", p_vkEnumeratePhysicalDevices(instance, &physical_count, NULL));
    if (!physical_count) { fprintf(stderr, "FAIL: selected ICD reports zero Vulkan physical devices.\n"); status = 7; goto cleanup; }
    physical_list = (VkPhysicalDevice *)calloc(physical_count, sizeof(*physical_list));
    if (!physical_list) { fprintf(stderr, "FAIL: out of host memory allocating physical-device list.\n"); status = 7; goto cleanup; }
    VK_TRY("vkEnumeratePhysicalDevices(list)", p_vkEnumeratePhysicalDevices(instance, &physical_count, physical_list));
    printf("Physical devices reported: %u\n", physical_count);
    for (i = 0; i < (int)physical_count; ++i) {
        VkPhysicalDeviceProperties p;
        p_vkGetPhysicalDeviceProperties(physical_list[i], &p);
        printf("  adapter[%d]: name=%s vendor=0x%04X device=0x%04X type=%s api=", i, p.deviceName, p.vendorID, p.deviceID, device_type_name(p.deviceType));
        print_version("", p.apiVersion); printf(" (compute eligibility is tested from queue flags below)\n");
    }

    /* Prefer compliant Intel hardware even in an explicitly relaxed sandbox run. */
    for (i = 0; i < (allow_non_intel ? 2 : 1) && !selected; ++i) {
        uint32_t n;
        for (n = 0; n < physical_count && !selected; ++n) {
            VkPhysicalDeviceProperties p; uint32_t candidate_family;
            p_vkGetPhysicalDeviceProperties(physical_list[n], &p);
            if (is_software_or_warp(&p)) { printf("REJECT adapter[%u]: CPU/WARP/software renderer policy (%s).\n", n, p.deviceName); continue; }
            if (VK_VERSION_MAJOR(p.apiVersion) < 1) { printf("REJECT adapter[%u]: reports API below Vulkan 1.0.\n", n); continue; }
            if (i == 0 && p.vendorID != INTEL_VENDOR_ID) { printf("REJECT adapter[%u]: vendor 0x%04X is not Intel 0x8086.\n", n, p.vendorID); continue; }
            if (i == 1 && p.vendorID == INTEL_VENDOR_ID) continue;
            printf("Candidate adapter[%u] queue-family test:\n", n);
            if (!find_unified_queue(physical_list[n], surface, &candidate_family)) { printf("REJECT adapter[%u]: no unified graphics+compute+present queue family.\n", n); continue; }
            if (!device_extension_present(physical_list[n], VK_KHR_SWAPCHAIN_EXTENSION_NAME, 0)) { printf("REJECT adapter[%u]: VK_KHR_swapchain not reported.\n", n); continue; }
            selected = physical_list[n]; family = candidate_family; selected_props = p;
        }
    }
    if (!selected) {
        fprintf(stderr, "FAIL: no eligible non-software Intel GPU with compute, VK_KHR_swapchain, and a unified graphics+compute+Win32-present queue was found.\n");
        fprintf(stderr, "      Non-Intel candidates require explicit --allow-non-intel-test; it never permits CPU/WARP/software.\n"); status = 8; goto cleanup;
    }
    if (selected_props.vendorID != INTEL_VENDOR_ID) printf("WARNING: selected non-Intel adapter under explicit override; result is NOT HD 5500 hardware validation.\n");
    printf("Selected queue family: %u (graphics + compute + present).\n", family);
    print_device_report(selected);
    p_vkGetPhysicalDeviceMemoryProperties(selected, &selected_memory);
    if (!device_extension_present(selected, VK_KHR_SWAPCHAIN_EXTENSION_NAME, 1)) { fprintf(stderr, "FAIL: selected device lost VK_KHR_swapchain during enumeration.\n"); status = 8; goto cleanup; }

    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &priority };
        const char *extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset" };
        VkDeviceCreateInfo dci;
        memset(&dci, 0, sizeof(dci)); dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = extensions; dci.pEnabledFeatures = NULL;
        if (!no_portability_optin && device_extension_present(selected, "VK_KHR_portability_subset", 0)) {
            dci.enabledExtensionCount = 2;
            printf("Portability: explicitly enabling the advertised VK_KHR_portability_subset device extension.\n");
        }
        VK_TRY("vkCreateDevice(Vulkan 1.0 core compute commands + VK_KHR_swapchain)", p_vkCreateDevice(selected, &dci, NULL, &device));
    }
    LOAD_DEVICE(device, vkDestroyDevice); LOAD_DEVICE(device, vkGetDeviceQueue); LOAD_DEVICE(device, vkCreateBuffer); LOAD_DEVICE(device, vkDestroyBuffer);
    LOAD_DEVICE(device, vkGetBufferMemoryRequirements); LOAD_DEVICE(device, vkAllocateMemory); LOAD_DEVICE(device, vkFreeMemory); LOAD_DEVICE(device, vkBindBufferMemory);
    LOAD_DEVICE(device, vkMapMemory); LOAD_DEVICE(device, vkUnmapMemory); LOAD_DEVICE(device, vkFlushMappedMemoryRanges); LOAD_DEVICE(device, vkInvalidateMappedMemoryRanges);
    LOAD_DEVICE(device, vkCreateDescriptorSetLayout); LOAD_DEVICE(device, vkDestroyDescriptorSetLayout); LOAD_DEVICE(device, vkCreateDescriptorPool); LOAD_DEVICE(device, vkDestroyDescriptorPool);
    LOAD_DEVICE(device, vkAllocateDescriptorSets); LOAD_DEVICE(device, vkUpdateDescriptorSets); LOAD_DEVICE(device, vkCreatePipelineLayout); LOAD_DEVICE(device, vkDestroyPipelineLayout);
    LOAD_DEVICE(device, vkCreateShaderModule); LOAD_DEVICE(device, vkDestroyShaderModule); LOAD_DEVICE(device, vkCreateComputePipelines); LOAD_DEVICE(device, vkDestroyPipeline);
    LOAD_DEVICE(device, vkCreateCommandPool); LOAD_DEVICE(device, vkDestroyCommandPool); LOAD_DEVICE(device, vkAllocateCommandBuffers); LOAD_DEVICE(device, vkBeginCommandBuffer); LOAD_DEVICE(device, vkEndCommandBuffer);
    LOAD_DEVICE(device, vkCmdPipelineBarrier); LOAD_DEVICE(device, vkCmdBindPipeline); LOAD_DEVICE(device, vkCmdBindDescriptorSets); LOAD_DEVICE(device, vkCmdDispatch); LOAD_DEVICE(device, vkCmdClearColorImage);
    LOAD_DEVICE(device, vkCreateFence); LOAD_DEVICE(device, vkDestroyFence); LOAD_DEVICE(device, vkWaitForFences); LOAD_DEVICE(device, vkCreateSemaphore); LOAD_DEVICE(device, vkDestroySemaphore);
    LOAD_DEVICE(device, vkQueueSubmit); LOAD_DEVICE(device, vkDeviceWaitIdle); LOAD_DEVICE(device, vkCreateSwapchainKHR); LOAD_DEVICE(device, vkDestroySwapchainKHR);
    LOAD_DEVICE(device, vkGetSwapchainImagesKHR); LOAD_DEVICE(device, vkAcquireNextImageKHR); LOAD_DEVICE(device, vkQueuePresentKHR);
    NEED(vkDestroyDevice); NEED(vkGetDeviceQueue); NEED(vkCreateBuffer); NEED(vkDestroyBuffer); NEED(vkGetBufferMemoryRequirements); NEED(vkAllocateMemory); NEED(vkFreeMemory); NEED(vkBindBufferMemory);
    NEED(vkMapMemory); NEED(vkUnmapMemory); NEED(vkFlushMappedMemoryRanges); NEED(vkInvalidateMappedMemoryRanges); NEED(vkCreateDescriptorSetLayout); NEED(vkDestroyDescriptorSetLayout);
    NEED(vkCreateDescriptorPool); NEED(vkDestroyDescriptorPool); NEED(vkAllocateDescriptorSets); NEED(vkUpdateDescriptorSets); NEED(vkCreatePipelineLayout); NEED(vkDestroyPipelineLayout);
    NEED(vkCreateShaderModule); NEED(vkDestroyShaderModule); NEED(vkCreateComputePipelines); NEED(vkDestroyPipeline); NEED(vkCreateCommandPool); NEED(vkDestroyCommandPool);
    NEED(vkAllocateCommandBuffers); NEED(vkBeginCommandBuffer); NEED(vkEndCommandBuffer); NEED(vkCmdPipelineBarrier); NEED(vkCmdBindPipeline); NEED(vkCmdBindDescriptorSets); NEED(vkCmdDispatch); NEED(vkCmdClearColorImage);
    NEED(vkCreateFence); NEED(vkDestroyFence); NEED(vkWaitForFences); NEED(vkCreateSemaphore); NEED(vkDestroySemaphore); NEED(vkQueueSubmit); NEED(vkDeviceWaitIdle);
    NEED(vkCreateSwapchainKHR); NEED(vkDestroySwapchainKHR); NEED(vkGetSwapchainImagesKHR); NEED(vkAcquireNextImageKHR); NEED(vkQueuePresentKHR);
    p_vkGetDeviceQueue(device, family, 0, &queue);
    if (!queue) { fprintf(stderr, "FAIL: vkGetDeviceQueue returned VK_NULL_HANDLE.\n"); status = 9; goto cleanup; }
    printf("PASS: created device with Vulkan 1.0 core compute commands (no optional features enabled) and obtained queue.\n");

    /* Compute: host-visible storage buffer -> explicit HOST/COMPUTE barriers -> 0x5500 readback. */
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        VkMemoryRequirements mr;
        VkMemoryAllocateInfo mai;
        VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo slci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &binding };
        VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
        VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &pool_size };
        VkDescriptorSetAllocateInfo dsai;
        VkPipelineLayoutCreateInfo plci;
        VkShaderModuleCreateInfo smci;
        VkPipelineShaderStageCreateInfo stage;
        VkComputePipelineCreateInfo cpci;
        VkCommandPoolCreateInfo cpoci;
        VkCommandBufferAllocateInfo cbai;
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
        VkDescriptorBufferInfo dbi;
        VkWriteDescriptorSet write;
        VkBufferMemoryBarrier to_compute, to_host;
        VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
        VkSubmitInfo submit;
        VK_TRY("vkCreateBuffer(storage buffer)", p_vkCreateBuffer(device, &bci, NULL, &result_buffer));
        p_vkGetBufferMemoryRequirements(device, result_buffer, &mr);
        selected_memory_type = choose_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &selected_memory);
        if (selected_memory_type < 0) selected_memory_type = choose_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &selected_memory);
        if (selected_memory_type < 0) { fprintf(stderr, "FAIL: no host-visible memory type is compatible with the compute result buffer.\n"); status = 10; goto cleanup; }
        coherent = (selected_memory.memoryTypes[selected_memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        printf("Compute readback buffer: allocationSize=%llu memoryType=%d flags=0x%X%s\n", (unsigned long long)mr.size, selected_memory_type, selected_memory.memoryTypes[selected_memory_type].propertyFlags, coherent ? " HOST_COHERENT" : " (explicit flush/invalidate required)");
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.pNext = NULL; mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)selected_memory_type;
        VK_TRY("vkAllocateMemory(compute result)", p_vkAllocateMemory(device, &mai, NULL, &result_memory));
        VK_TRY("vkBindBufferMemory(compute result)", p_vkBindBufferMemory(device, result_buffer, result_memory, 0));
        VK_TRY("vkMapMemory(compute result)", p_vkMapMemory(device, result_memory, 0, mr.size, 0, &mapped));
        if (!mapped) { fprintf(stderr, "FAIL: vkMapMemory returned VK_SUCCESS but a null mapped pointer; no dereference attempted.\n"); status = 10; goto cleanup; }
        printf("PASS: compute result memory mapped to a non-null pointer.\n");
        *(uint32_t *)mapped = 0;
        if (!coherent) { VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, result_memory, 0, VK_WHOLE_SIZE }; VK_TRY("vkFlushMappedMemoryRanges(initial zero)", p_vkFlushMappedMemoryRanges(device, 1, &range)); }
        VK_TRY("vkCreateDescriptorSetLayout(storage buffer)", p_vkCreateDescriptorSetLayout(device, &slci, NULL, &set_layout));
        VK_TRY("vkCreateDescriptorPool", p_vkCreateDescriptorPool(device, &dpci, NULL, &descriptor_pool));
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsai.pNext = NULL; dsai.descriptorPool = descriptor_pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &set_layout;
        VK_TRY("vkAllocateDescriptorSets", p_vkAllocateDescriptorSets(device, &dsai, &descriptor_set));
        dbi.buffer = result_buffer; dbi.offset = 0; dbi.range = sizeof(uint32_t);
        memset(&write, 0, sizeof(write)); write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = descriptor_set; write.dstBinding = 0; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &dbi;
        p_vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; plci.pNext = NULL; plci.flags = 0; plci.setLayoutCount = 1; plci.pSetLayouts = &set_layout; plci.pushConstantRangeCount = 0; plci.pPushConstantRanges = NULL;
        VK_TRY("vkCreatePipelineLayout", p_vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout));
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; smci.pNext = NULL; smci.flags = 0; smci.codeSize = probe_compute_5500_spv_size; smci.pCode = probe_compute_5500_spv;
        VK_TRY("vkCreateShaderModule(embedded SPIR-V 1.0)", p_vkCreateShaderModule(device, &smci, NULL, &shader_module));
        memset(&stage, 0, sizeof(stage)); stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = shader_module; stage.pName = "main";
        memset(&cpci, 0, sizeof(cpci)); cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; cpci.stage = stage; cpci.layout = pipeline_layout;
        VK_TRY("vkCreateComputePipelines", p_vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &compute_pipeline));
        cpoci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpoci.pNext = NULL; cpoci.flags = 0; cpoci.queueFamilyIndex = family;
        VK_TRY("vkCreateCommandPool", p_vkCreateCommandPool(device, &cpoci, NULL, &command_pool));
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cbai.pNext = NULL; cbai.commandPool = command_pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 2;
        VK_TRY("vkAllocateCommandBuffers", p_vkAllocateCommandBuffers(device, &cbai, commands));
        VK_TRY("vkCreateFence(compute)", p_vkCreateFence(device, &fci, NULL, &compute_fence));
        VK_TRY("vkBeginCommandBuffer(compute)", p_vkBeginCommandBuffer(commands[0], &cbbi));
        memset(&to_compute, 0, sizeof(to_compute)); to_compute.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; to_compute.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; to_compute.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; to_compute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_compute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_compute.buffer = result_buffer; to_compute.offset = 0; to_compute.size = sizeof(uint32_t);
        p_vkCmdPipelineBarrier(commands[0], VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &to_compute, 0, NULL);
        p_vkCmdBindPipeline(commands[0], VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
        p_vkCmdBindDescriptorSets(commands[0], VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
        p_vkCmdDispatch(commands[0], 1, 1, 1);
        memset(&to_host, 0, sizeof(to_host)); to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT; to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_host.buffer = result_buffer; to_host.offset = 0; to_host.size = sizeof(uint32_t);
        p_vkCmdPipelineBarrier(commands[0], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &to_host, 0, NULL);
        VK_TRY("vkEndCommandBuffer(compute)", p_vkEndCommandBuffer(commands[0]));
        memset(&submit, 0, sizeof(submit)); submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &commands[0];
        VK_TRY("vkQueueSubmit(compute)", p_vkQueueSubmit(queue, 1, &submit, compute_fence));
        VK_TRY("vkWaitForFences(compute)", p_vkWaitForFences(device, 1, &compute_fence, VK_TRUE, UINT64_MAX));
        if (!coherent) { VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, result_memory, 0, VK_WHOLE_SIZE }; VK_TRY("vkInvalidateMappedMemoryRanges(compute readback)", p_vkInvalidateMappedMemoryRanges(device, 1, &range)); }
        printf("Compute readback actual value=0x%08X (expected 0x00005500)\n", *(uint32_t *)mapped);
        if (*(uint32_t *)mapped != 0x00005500u) { fprintf(stderr, "FAIL: compute shader completed but storage-buffer readback did not equal 0x00005500.\n"); status = 11; goto cleanup; }
        printf("PASS: Vulkan 1.0 descriptor-set/storage-buffer compute dispatch produced 0x00005500 with explicit barriers.\n");
    }

    /* WSI: intentionally request exactly one FIFO swapchain image, matching Ashes D3D11 surface caps. */
    {
        VkSurfaceCapabilitiesKHR caps;
        uint32_t count = 0, image_index = 0;
        VkSurfaceFormatKHR *formats = NULL, chosen_format;
        VkPresentModeKHR *modes = NULL;
        int fifo = 0;
        VkExtent2D extent;
        VkCompositeAlphaFlagBitsKHR alpha = 0;
        VkSwapchainCreateInfoKHR sci;
        VkSemaphoreCreateInfo semci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0 };
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
        VkImage swap_image;
        VkImageSubresourceRange image_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier barrier;
        VkClearColorValue clear = { { 0.08f, 0.55f, 0.20f, 1.0f } };
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit;
        VkPresentInfoKHR present;
        VK_TRY("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(selected, surface, &caps));
        printf("Surface capabilities: minImageCount=%u maxImageCount=%u currentExtent=%ux%u minExtent=%ux%u maxExtent=%ux%u usage=0x%X transforms=0x%X compositeAlpha=0x%X\n", caps.minImageCount, caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height, caps.minImageExtent.width, caps.minImageExtent.height, caps.maxImageExtent.width, caps.maxImageExtent.height, caps.supportedUsageFlags, caps.supportedTransforms, caps.supportedCompositeAlpha);
        if (caps.minImageCount > 1 || (caps.maxImageCount && caps.maxImageCount < 1)) { fprintf(stderr, "FAIL: one-image FIFO contract is unsupported by reported surface caps (min=%u max=%u); probe will not substitute a multi-image swapchain.\n", caps.minImageCount, caps.maxImageCount); status = 12; goto cleanup; }
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { fprintf(stderr, "FAIL: surface does not support VK_IMAGE_USAGE_TRANSFER_DST_BIT needed for an actual clear.\n"); status = 12; goto cleanup; }
        VK_TRY("vkGetPhysicalDeviceSurfaceFormatsKHR(count)", p_vkGetPhysicalDeviceSurfaceFormatsKHR(selected, surface, &count, NULL));
        if (!count) { fprintf(stderr, "FAIL: surface reports no formats.\n"); status = 12; goto cleanup; }
        formats = (VkSurfaceFormatKHR *)calloc(count, sizeof(*formats)); if (!formats) { fprintf(stderr, "FAIL: out of host memory for surface formats.\n"); status = 12; goto cleanup; }
        VK_TRY("vkGetPhysicalDeviceSurfaceFormatsKHR(list)", p_vkGetPhysicalDeviceSurfaceFormatsKHR(selected, surface, &count, formats));
        chosen_format = formats[0]; if (chosen_format.format == VK_FORMAT_UNDEFINED) chosen_format.format = VK_FORMAT_B8G8R8A8_UNORM;
        for (uint32_t f = 0; f < count; ++f) if (formats[f].format == VK_FORMAT_R8G8B8A8_UNORM) { chosen_format = formats[f]; break; }
        for (uint32_t f = 0; f < count; ++f) if (formats[f].format == VK_FORMAT_B8G8R8A8_UNORM) { chosen_format = formats[f]; break; }
        printf("Selected surface format: format=%d colorSpace=%d\n", chosen_format.format, chosen_format.colorSpace); free(formats); formats = NULL;
        VK_TRY("vkGetPhysicalDeviceSurfacePresentModesKHR(count)", p_vkGetPhysicalDeviceSurfacePresentModesKHR(selected, surface, &count, NULL));
        modes = count ? (VkPresentModeKHR *)calloc(count, sizeof(*modes)) : NULL; if (!count || !modes) { fprintf(stderr, "FAIL: surface reports no present modes or allocation failed.\n"); status = 12; goto cleanup; }
        VK_TRY("vkGetPhysicalDeviceSurfacePresentModesKHR(list)", p_vkGetPhysicalDeviceSurfacePresentModesKHR(selected, surface, &count, modes));
        printf("Surface present modes:"); for (i = 0; i < (int)count; ++i) { printf(" %d", modes[i]); if (modes[i] == VK_PRESENT_MODE_FIFO_KHR) fifo = 1; } printf("\n"); free(modes); modes = NULL;
        if (!fifo) { fprintf(stderr, "FAIL: VK_PRESENT_MODE_FIFO_KHR is not reported.\n"); status = 12; goto cleanup; }
        if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
        else {
            RECT r; GetClientRect(window, &r); extent.width = (uint32_t)(r.right - r.left); extent.height = (uint32_t)(r.bottom - r.top);
            if (caps.minImageExtent.width <= caps.maxImageExtent.width && caps.minImageExtent.height <= caps.maxImageExtent.height) {
                if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
                if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
                if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
                if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
            } else printf("WARNING: ICD reported inverted variable extent bounds; using actual client extent %ux%u without inventing caps.\n", extent.width, extent.height);
        }
        if (!extent.width || !extent.height) { fprintf(stderr, "FAIL: selected surface has zero extent.\n"); status = 12; goto cleanup; }
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) alpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        else { fprintf(stderr, "FAIL: surface reports no usable composite alpha mode.\n"); status = 12; goto cleanup; }
        memset(&sci, 0, sizeof(sci)); sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR; sci.surface = surface; sci.minImageCount = 1; sci.imageFormat = chosen_format.format; sci.imageColorSpace = chosen_format.colorSpace; sci.imageExtent = extent; sci.imageArrayLayers = 1; sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; sci.preTransform = caps.currentTransform; sci.compositeAlpha = alpha; sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; sci.clipped = VK_TRUE;
        printf("Requesting swapchain: exactly 1 image, FIFO, extent=%ux%u, transfer-dst clear.\n", extent.width, extent.height);
        VK_TRY("vkCreateSwapchainKHR(one-image FIFO)", p_vkCreateSwapchainKHR(device, &sci, NULL, &swapchain));
        VK_TRY("vkGetSwapchainImagesKHR(count)", p_vkGetSwapchainImagesKHR(device, swapchain, &count, NULL));
        if (count != 1) { fprintf(stderr, "FAIL: one-image swapchain was requested but ICD returned %u images; incompatible with the Ashes one-image contract.\n", count); status = 12; goto cleanup; }
        VK_TRY("vkGetSwapchainImagesKHR(image)", p_vkGetSwapchainImagesKHR(device, swapchain, &count, &swap_image));
        VK_TRY("vkCreateSemaphore(acquired)", p_vkCreateSemaphore(device, &semci, NULL, &acquired)); VK_TRY("vkCreateSemaphore(rendered)", p_vkCreateSemaphore(device, &semci, NULL, &rendered));
        VK_TRY("vkAcquireNextImageKHR", p_vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &image_index));
        if (image_index != 0) { fprintf(stderr, "FAIL: one-image swapchain acquired unexpected image index %u.\n", image_index); status = 12; goto cleanup; }
        VK_TRY("vkBeginCommandBuffer(present)", p_vkBeginCommandBuffer(commands[1], &begin));
        memset(&barrier, 0, sizeof(barrier)); barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.image = swap_image; barrier.subresourceRange = image_range; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fprintf(stderr, "CALL: vkCmdPipelineBarrier(swapchain to transfer)\n"); fflush(stderr);
        p_vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        fprintf(stderr, "OK: swapchain first image barrier; CALL: vkCmdClearColorImage\n"); fflush(stderr);
        p_vkCmdClearColorImage(commands[1], swap_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &image_range);
        fprintf(stderr, "OK: vkCmdClearColorImage\n"); fflush(stderr);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
        fprintf(stderr, "CALL: vkCmdPipelineBarrier(swapchain to present)\n"); fflush(stderr);
        p_vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        fprintf(stderr, "OK: swapchain final image barrier\n"); fflush(stderr);
        VK_TRY("vkEndCommandBuffer(present)", p_vkEndCommandBuffer(commands[1]));
        memset(&submit, 0, sizeof(submit)); submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &wait_stage; submit.commandBufferCount = 1; submit.pCommandBuffers = &commands[1]; submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &rendered;
        VK_TRY("vkQueueSubmit(clear frame)", p_vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        memset(&present, 0, sizeof(present)); present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR; present.waitSemaphoreCount = 1; present.pWaitSemaphores = &rendered; present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &image_index;
        VK_TRY("vkQueuePresentKHR(actual cleared frame)", p_vkQueuePresentKHR(queue, &present));
        VK_TRY("vkDeviceWaitIdle(after present)", p_vkDeviceWaitIdle(device));
        printf("PASS: presented an actual green cleared frame to the visible Win32 window through one-image FIFO WSI.\n");
        printf("Window remains visible for 1.5 seconds so the presentation can be observed.\n"); pump_messages(1500);
    }
    printf("PASS: probe complete. These are real results from the supplied loader and per-process ICD selection; this is not a RimWorld game launch.\n");
    status = 0;

cleanup:
    if (device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(device);
    if (mapped && p_vkUnmapMemory) p_vkUnmapMemory(device, result_memory);
    if (swapchain && p_vkDestroySwapchainKHR) p_vkDestroySwapchainKHR(device, swapchain, NULL);
    if (acquired && p_vkDestroySemaphore) p_vkDestroySemaphore(device, acquired, NULL);
    if (rendered && p_vkDestroySemaphore) p_vkDestroySemaphore(device, rendered, NULL);
    if (compute_fence && p_vkDestroyFence) p_vkDestroyFence(device, compute_fence, NULL);
    if (command_pool && p_vkDestroyCommandPool) p_vkDestroyCommandPool(device, command_pool, NULL);
    if (compute_pipeline && p_vkDestroyPipeline) p_vkDestroyPipeline(device, compute_pipeline, NULL);
    if (shader_module && p_vkDestroyShaderModule) p_vkDestroyShaderModule(device, shader_module, NULL);
    if (pipeline_layout && p_vkDestroyPipelineLayout) p_vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (descriptor_pool && p_vkDestroyDescriptorPool) p_vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (set_layout && p_vkDestroyDescriptorSetLayout) p_vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (result_buffer && p_vkDestroyBuffer) p_vkDestroyBuffer(device, result_buffer, NULL);
    if (result_memory && p_vkFreeMemory) p_vkFreeMemory(device, result_memory, NULL);
    if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL);
    if (surface && p_vkDestroySurfaceKHR) p_vkDestroySurfaceKHR(instance, surface, NULL);
    if (window) DestroyWindow(window);
    if (class_atom) UnregisterClassW(L"AshesVulkan10GpuIcdProbeWindow", GetModuleHandleW(NULL));
    free(physical_list);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    return status;
}
