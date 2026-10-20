/*
 * ImagePoolLifetimeProbe: raw Vulkan lifetime fixture for the Ashes D3D11 ICD.
 *
 * No Vulkan symbol is imported. The executable validates every function it
 * obtains from a caller-supplied absolute loader and sets the caller-supplied
 * ICD only in this process. It exercises repeated sampled 2D image/view
 * creation at one non-zero parent allocation offset, destroys each view before
 * its image while retaining the parent, then performs a real host-visible
 * image upload/copy/readback byte comparison. The final unused-image control
 * deliberately frees bound memory before destroying that image.
 *
 * This fixture observes Vulkan API behavior only. It does not, and cannot,
 * claim an exact COM-reference or GPU-memory release count from public Vulkan.
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
#define KiB (1024ull)
#define MiB (1024ull * 1024ull)
#define IMAGE_POOL_PARENT_BYTES (4ull * MiB)
#define IMAGE_POOL_FIXED_OFFSET (64ull * KiB)
#define IMAGE_POOL_CYCLES 6u
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
static PFN_vkCreateImageView p_vkCreateImageView;
static PFN_vkDestroyImageView p_vkDestroyImageView;
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

static LONG g_debug_total;
static LONG g_debug_errors;
static LONG g_debug_suspect_text;
static LONG g_preflight_failures;
static const char *g_first_failure;
static const char *g_current_operation;
static const char *g_first_debug_error_operation;

#define LOAD_GLOBAL(n) p_##n = (PFN_##n)gipa(VK_NULL_HANDLE, #n)
#define LOAD_INSTANCE(i,n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d,n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)
#define NEED(n) do { if (!p_##n) { ERR("FAIL: required entry point unavailable: %s\n", #n); status = 20; if (!g_first_failure) g_first_failure = #n " unavailable"; goto cleanup; } } while (0)
#define VK_CALL(label, expr) do { \
    LOG("CALL: %s\n", (label)); \
    VkResult call_result_ = (expr); \
    LOG("RESULT: %s => %s (%d)\n", (label), result_name(call_result_), (int)call_result_); \
    if (call_result_ != VK_SUCCESS) { if (!g_first_failure) g_first_failure = (label); status = 21; goto cleanup; } \
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

static VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    return alignment ? ((value + alignment - 1u) / alignment) * alignment : value;
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

static int find_transfer_queue(VkPhysicalDevice physical, uint32_t *family_out) {
    uint32_t count = 0, i;
    VkQueueFamilyProperties *props = NULL;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (!count) return 0;
    props = (VkQueueFamilyProperties *)calloc(count, sizeof(*props));
    if (!props) return 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, props);
    for (i = 0; i < count; ++i) {
        LOG("QUEUE[%u]: flags=0x%X count=%u\n", i, props[i].queueFlags, props[i].queueCount);
        if (props[i].queueCount && (props[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
            *family_out = i;
            free(props);
            return 1;
        }
    }
    free(props);
    return 0;
}

static int choose_memory_type(uint32_t type_bits, const VkPhysicalDeviceMemoryProperties *props,
                              VkMemoryPropertyFlags required) {
    uint32_t i;
    for (i = 0; i < props->memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (props->memoryTypes[i].propertyFlags & required) == required)
            return (int)i;
    return -1;
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
        if (!g_first_debug_error_operation) g_first_debug_error_operation = g_current_operation ? g_current_operation : "operation not tagged";
    }
    if (!text_is_sane(id_name) || !text_is_sane(message) ||
        ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) && (!data || !data->pMessage || !data->pMessage[0])))
        InterlockedIncrement(&g_debug_suspect_text);
    LOG("DEBUG_UTILS[%ld]: severity=0x%X type=0x%X code=%d id=%s message=%s\n",
        number, (unsigned)severity, (unsigned)types, data ? data->messageIdNumber : 0, id_name, message);
    return VK_FALSE;
}

static VkImageCreateInfo image_info_for(VkImageUsageFlags usage) {
    VkImageCreateInfo info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent.width = 2;
    info.extent.height = 2;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

/* This intentionally continues after an error so every usage/memory isolate is logged. */
static VkResult preflight_create_bind_image(VkDevice device, const VkPhysicalDeviceMemoryProperties *mem_props,
                                             VkImageUsageFlags usage, VkMemoryPropertyFlags required,
                                             const char *label, int nonzero_bind_offset) {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateInfo info = image_info_for(usage);
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc;
    VkDeviceSize bind_offset = 0;
    VkResult r;
    int type;
    LOG("CALL: vkCreateImage(%s; usage=0x%X; optimal RGBA8 2x2) [preflight]\n", label, usage);
    r = p_vkCreateImage(device, &info, NULL, &image);
    LOG("RESULT: vkCreateImage(%s preflight) => %s (%d)\n", label, result_name(r), (int)r);
    if (r != VK_SUCCESS) return r;
    p_vkGetImageMemoryRequirements(device, image, &req);
    LOG("REQUIREMENTS %s: size=%llu alignment=%llu typeBits=0x%08X\n", label,
        (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits);
    type = choose_memory_type(req.memoryTypeBits, mem_props, required);
    if (type < 0) {
        ERR("RESULT: preflight %s => no compatible required memory type flags=0x%X (not substituted).\n", label, required);
        p_vkDestroyImage(device, image, NULL);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (nonzero_bind_offset) bind_offset = align_up(req.alignment, req.alignment);
    memset(&alloc, 0, sizeof(alloc));
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = bind_offset + req.size;
    alloc.memoryTypeIndex = (uint32_t)type;
    LOG("CALL: vkAllocateMemory(%s requiredFlags=0x%X type=%d actualFlags=0x%X size=%llu) [preflight]\n", label, required, type,
        mem_props->memoryTypes[type].propertyFlags, (unsigned long long)alloc.allocationSize);
    r = p_vkAllocateMemory(device, &alloc, NULL, &memory);
    LOG("RESULT: vkAllocateMemory(%s preflight) => %s (%d)\n", label, result_name(r), (int)r);
    if (r == VK_SUCCESS) {
        LOG("CALL: vkBindImageMemory(%s, offset=%llu) [preflight]\n", label, (unsigned long long)bind_offset);
        r = p_vkBindImageMemory(device, image, memory, bind_offset);
        LOG("RESULT: vkBindImageMemory(%s preflight) => %s (%d)\n", label, result_name(r), (int)r);
    }
    if (memory) p_vkFreeMemory(device, memory, NULL);
    if (image) p_vkDestroyImage(device, image, NULL);
    return r;
}

static void run_host_visible_image_preflights(VkDevice device, const VkPhysicalDeviceMemoryProperties *mem_props) {
	VkResult r;
	LOG("HOST_VISIBLE IMAGE PREFLIGHTS: exercising advertised host-visible image type and nonzero image-memory binding.\n");
    r = preflight_create_bind_image(device, mem_props, VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        "sampled-only HOST_VISIBLE|HOST_COHERENT (nonzero image bind)", 1);
    if (r != VK_SUCCESS) { InterlockedIncrement(&g_preflight_failures); LOG("PREFLIGHT RESULT: sampled-only HOST_VISIBLE create/bind failed.\n"); }
    r = preflight_create_bind_image(device, mem_props, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        "sampled|transfer-src|transfer-dst HOST_VISIBLE|HOST_COHERENT (nonzero image bind)", 1);
	if (r != VK_SUCCESS) { InterlockedIncrement(&g_preflight_failures); LOG("PREFLIGHT RESULT: combined HOST_VISIBLE create/bind failed.\n"); }
}

static VkResult log_image_call(const char *label, VkResult result) {
    LOG("RESULT: %s => %s (%d)\n", label, result_name(result), (int)result);
    return result;
}

static VkResult run_sampled_image_pool_reuse(VkDevice device,
                                               const VkPhysicalDeviceMemoryProperties *mem_props) {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory parent = VK_NULL_HANDLE;
    VkImageCreateInfo image_info = image_info_for(VK_IMAGE_USAGE_SAMPLED_BIT);
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc;
    VkDeviceSize offset;
    VkResult r = VK_SUCCESS;
    uint32_t i;
    int type;

    memset(&req, 0, sizeof(req));
    LOG("LIFETIME_SCOPE: %u sampled 2D image/view cycles use one retained %llu-byte parent at one nonzero offset.\n",
        IMAGE_POOL_CYCLES, (unsigned long long)IMAGE_POOL_PARENT_BYTES);
    r = log_image_call("vkCreateImage(pool requirements sample)",
        p_vkCreateImage(device, &image_info, NULL, &image));
    if (r != VK_SUCCESS) goto done;
    p_vkGetImageMemoryRequirements(device, image, &req);
    offset = align_up(IMAGE_POOL_FIXED_OFFSET, req.alignment);
    LOG("POOL_REQUIREMENTS: size=%llu alignment=%llu typeBits=0x%08X fixedOffset=%llu\n",
        (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits,
        (unsigned long long)offset);
    if (!offset || offset > IMAGE_POOL_PARENT_BYTES || req.size > IMAGE_POOL_PARENT_BYTES - offset) {
        ERR("FAIL: sampled image requirements do not fit the 4 MiB retained parent at a nonzero offset.\n");
        r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        goto done;
    }
    type = choose_memory_type(req.memoryTypeBits, mem_props, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) {
        ERR("FAIL: sampled pool image exposes no DEVICE_LOCAL memory type; no substitute is used.\n");
        r = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    p_vkDestroyImage(device, image, NULL);
    image = VK_NULL_HANDLE;
    memset(&alloc, 0, sizeof(alloc));
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = IMAGE_POOL_PARENT_BYTES;
    alloc.memoryTypeIndex = (uint32_t)type;
    r = log_image_call("vkAllocateMemory(retained 4MiB sampled-image parent)",
        p_vkAllocateMemory(device, &alloc, NULL, &parent));
    if (r != VK_SUCCESS) goto done;

    for (i = 0; i < IMAGE_POOL_CYCLES; ++i) {
        VkImageViewCreateInfo view_info;
        char label[128];
        r = log_image_call("vkCreateImage(reused sampled 2D child)",
            p_vkCreateImage(device, &image_info, NULL, &image));
        if (r != VK_SUCCESS) goto done;
        p_vkGetImageMemoryRequirements(device, image, &req);
        if (req.alignment == 0u || offset % req.alignment != 0u ||
            req.size > IMAGE_POOL_PARENT_BYTES - offset ||
            !(req.memoryTypeBits & (1u << (uint32_t)type))) {
            ERR("FAIL: reused sampled child[%u] does not remain compatible with retained parent/type/offset.\n", i);
            r = VK_ERROR_FEATURE_NOT_PRESENT;
            goto done;
        }
        snprintf(label, sizeof(label), "vkBindImageMemory(reused sampled child[%u], retained nonzero offset=%llu)",
            i, (unsigned long long)offset);
        r = log_image_call(label, p_vkBindImageMemory(device, image, parent, offset));
        if (r != VK_SUCCESS) goto done;
        memset(&view_info, 0, sizeof(view_info));
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1u;
        view_info.subresourceRange.layerCount = 1u;
        r = log_image_call("vkCreateImageView(reused sampled child)",
            p_vkCreateImageView(device, &view_info, NULL, &view));
        if (r != VK_SUCCESS) goto done;
        p_vkDestroyImageView(device, view, NULL);
        view = VK_NULL_HANDLE;
        p_vkDestroyImage(device, image, NULL);
        image = VK_NULL_HANDLE;
        LOG("POOL_CYCLE_COMPLETE: %u/%u; destroyed view before image; retained parent remains live.\n",
            i + 1u, IMAGE_POOL_CYCLES);
    }
    LOG("POOL_CYCLES_COMPLETE: Vulkan calls completed; public Vulkan cannot report the ICD's COM release count.\n");

done:
    if (view) p_vkDestroyImageView(device, view, NULL);
    if (image) p_vkDestroyImage(device, image, NULL);
    if (parent) p_vkFreeMemory(device, parent, NULL);
    return r;
}

static VkResult run_unused_image_parent_free_first_control(VkDevice device,
                                                            const VkPhysicalDeviceMemoryProperties *mem_props) {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateInfo info = image_info_for(VK_IMAGE_USAGE_SAMPLED_BIT);
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc;
    VkResult r;
    int type;

    memset(&req, 0, sizeof(req));
    r = log_image_call("vkCreateImage(unused parent-free-first control)",
        p_vkCreateImage(device, &info, NULL, &image));
    if (r != VK_SUCCESS) goto done;
    p_vkGetImageMemoryRequirements(device, image, &req);
    type = choose_memory_type(req.memoryTypeBits, mem_props, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) {
        ERR("FAIL: unused control image exposes no DEVICE_LOCAL memory type; no substitute is used.\n");
        r = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    memset(&alloc, 0, sizeof(alloc));
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = (uint32_t)type;
    r = log_image_call("vkAllocateMemory(unused parent-free-first control)",
        p_vkAllocateMemory(device, &alloc, NULL, &memory));
    if (r != VK_SUCCESS) goto done;
    r = log_image_call("vkBindImageMemory(unused parent-free-first control)",
        p_vkBindImageMemory(device, image, memory, 0u));
    if (r != VK_SUCCESS) goto done;
    p_vkFreeMemory(device, memory, NULL);
    memory = VK_NULL_HANDLE;
    p_vkDestroyImage(device, image, NULL);
    image = VK_NULL_HANDLE;
    LOG("PARENT_FREE_FIRST_UNUSED_IMAGE_CONTROL: bound but unused memory was freed before image destruction.\n");

done:
    if (memory) p_vkFreeMemory(device, memory, NULL);
    if (image) p_vkDestroyImage(device, image, NULL);
    return r;
}

static void usage(const wchar_t *exe) {
	LOG("Usage:\n  %ls --loader C:\\absolute\\vulkan-1.dll --icd C:\\absolute\\ashesD3D11Renderer_icd.json [--allow-non-intel-test]\n", exe);
	LOG("Both paths are mandatory absolute Windows paths. Default selection requires a non-software Intel adapter.\n");
	LOG("--allow-non-intel-test is only for the Wine/synthetic sandbox and is not HD 5500 validation.\n");
	LOG("The run retains a 4 MiB parent through six sampled image/view reuse cycles, then requires a host-visible image upload/readback byte comparison.\n");
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL;
    int allow_non_intel = 0, i, status = 1;
    HMODULE loader = NULL;
    union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } loader_gipa;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    int debug_utils_enabled = 0;
    VkPhysicalDevice *physical_list = NULL, physical = VK_NULL_HANDLE;
    uint32_t physical_count = 0, queue_family = UINT32_MAX;
    VkPhysicalDeviceProperties selected_props;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkBuffer upload_buffer = VK_NULL_HANDLE, readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE, readback_memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkDeviceMemory mapped_memory = VK_NULL_HANDLE;
    VkDeviceSize upload_bind_offset = 0, readback_bind_offset = 0;
    VkMemoryRequirements image_req, upload_req, readback_req;
    int image_type, upload_type, readback_type;
    static const uint8_t pixels[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x0F
    };

    memset(&selected_props, 0, sizeof(selected_props));
    memset(&mem_props, 0, sizeof(mem_props));
    memset(&image_req, 0, sizeof(image_req));
    memset(&upload_req, 0, sizeof(upload_req));
    memset(&readback_req, 0, sizeof(readback_req));

    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { usage(argv[0]); return 0; }
        else { ERR("FAIL: unrecognized or incomplete argument: %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!loader_path || !icd_path || !is_absolute_windows_path(loader_path) || !is_absolute_windows_path(icd_path)) {
        ERR("FAIL: --loader and --icd are mandatory absolute Windows paths.\n"); usage(argv[0]); return 2;
    }
    if (GetFileAttributesW(loader_path) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(icd_path) == INVALID_FILE_ATTRIBUTES) {
        ERR("FAIL: supplied loader or ICD path does not exist (GetLastError=%lu).\n", GetLastError()); return 2;
    }
    if (GetModuleHandleW(L"vulkan-1.dll")) {
        ERR("FAIL: vulkan-1.dll was already loaded; refusing ambiguous ICD isolation.\n"); return 3;
    }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path) ||
        !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) {
        ERR("FAIL: cannot set isolated per-process ICD environment (GetLastError=%lu).\n", GetLastError()); return 3;
    }

    LOG("Ashes D3D11 ImagePoolLifetimeProbe (raw Vulkan; no VMA)\n");
    LOG("LOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    LOG("ICD isolation: VK_ICD_FILENAMES and VK_DRIVER_FILES set in this process only.\n");
    if (allow_non_intel)
        LOG("WARNING: --allow-non-intel-test accepts Wine/synthetic/non-Intel only for sandbox integration; NOT HD 5500 validation.\n");

    loader = LoadLibraryW(loader_path);
    if (!loader) { ERR("FAIL: LoadLibraryW(loader) error %lu\n", GetLastError()); status = 4; g_first_failure = "LoadLibraryW(loader)"; goto cleanup; }
    loader_gipa.raw = GetProcAddress(loader, "vkGetInstanceProcAddr");
    gipa = loader_gipa.typed;
    if (!gipa) { ERR("FAIL: loader exposes no vkGetInstanceProcAddr.\n"); status = 4; g_first_failure = "vkGetInstanceProcAddr"; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion); LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties); LOAD_GLOBAL(vkCreateInstance);
    NEED(vkEnumerateInstanceExtensionProperties); NEED(vkCreateInstance);
    if (p_vkEnumerateInstanceVersion) {
        uint32_t version = VK_API_VERSION_1_0;
        VK_CALL("vkEnumerateInstanceVersion", p_vkEnumerateInstanceVersion(&version));
        LOG("LOADER MAX API: %u.%u.%u; application requests 1.0.0\n", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    }
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL,
            "Ashes image pool lifetime probe", 1u, "standalone", 1u, VK_API_VERSION_1_0 };
        VkInstanceCreateInfo create;
        VkDebugUtilsMessengerCreateInfoEXT debug_create;
        const char *extensions[2];
        uint32_t extension_count = 0;
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
            debug_create.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug_create.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug_create.pfnUserCallback = debug_callback;
            create.pNext = &debug_create;
            LOG("DEBUG_UTILS: VK_EXT_debug_utils advertised; enabling callback at instance creation.\n");
        } else LOG("DEBUG_UTILS: VK_EXT_debug_utils not advertised by selected loader/ICD; no callback available.\n");
        create.enabledExtensionCount = extension_count;
        create.ppEnabledExtensionNames = extensions;
        VK_CALL("vkCreateInstance(api=1.0)", p_vkCreateInstance(&create, NULL, &instance));
    }
    LOAD_INSTANCE(instance, vkDestroyInstance); LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceFormatProperties);
    LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties); LOAD_INSTANCE(instance, vkCreateDevice);
    LOAD_INSTANCE(instance, vkGetDeviceProcAddr); LOAD_INSTANCE(instance, vkCreateDebugUtilsMessengerEXT);
    LOAD_INSTANCE(instance, vkDestroyDebugUtilsMessengerEXT);
    NEED(vkDestroyInstance); NEED(vkEnumeratePhysicalDevices); NEED(vkGetPhysicalDeviceProperties);
    NEED(vkGetPhysicalDeviceMemoryProperties); NEED(vkGetPhysicalDeviceQueueFamilyProperties); NEED(vkGetPhysicalDeviceFormatProperties);
    NEED(vkEnumerateDeviceExtensionProperties); NEED(vkCreateDevice); NEED(vkGetDeviceProcAddr);
    if (debug_utils_enabled) {
        VkDebugUtilsMessengerCreateInfoEXT info;
        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = debug_callback;
        if (!p_vkCreateDebugUtilsMessengerEXT || !p_vkDestroyDebugUtilsMessengerEXT) {
            ERR("DEBUG_UTILS: extension advertised but messenger entry points unavailable.\n");
        } else {
            VK_CALL("vkCreateDebugUtilsMessengerEXT", p_vkCreateDebugUtilsMessengerEXT(instance, &info, NULL, &debug_messenger));
        }
    }

    VK_CALL("vkEnumeratePhysicalDevices(count)", p_vkEnumeratePhysicalDevices(instance, &physical_count, NULL));
    if (!physical_count) { ERR("FAIL: ICD reports no physical devices.\n"); status = 5; g_first_failure = "vkEnumeratePhysicalDevices: zero adapters"; goto cleanup; }
    physical_list = (VkPhysicalDevice *)calloc(physical_count, sizeof(*physical_list));
    if (!physical_list) { ERR("FAIL: host allocation for physical devices.\n"); status = 5; g_first_failure = "physical-device list allocation"; goto cleanup; }
    VK_CALL("vkEnumeratePhysicalDevices(list)", p_vkEnumeratePhysicalDevices(instance, &physical_count, physical_list));
    for (i = 0; i < (int)physical_count; ++i) {
        VkPhysicalDeviceProperties props;
        uint32_t candidate_family;
        p_vkGetPhysicalDeviceProperties(physical_list[i], &props);
        LOG("ADAPTER[%d]: name=%s vendor=0x%04X device=0x%04X type=%s%s\n", i, props.deviceName, props.vendorID, props.deviceID,
            device_type_name(props.deviceType), looks_software(&props) ? " [software-like]" : "");
        if (!allow_non_intel && (props.vendorID != INTEL_VENDOR_ID || looks_software(&props))) continue;
        if (!physical && find_transfer_queue(physical_list[i], &candidate_family)) {
            physical = physical_list[i]; queue_family = candidate_family; selected_props = props;
        }
    }
    if (!physical) { ERR("FAIL: no eligible transfer-queue adapter. Default requires non-software Intel; use --allow-non-intel-test only in Wine sandbox.\n"); status = 6; g_first_failure = "eligible Intel adapter selection"; goto cleanup; }
    if (selected_props.vendorID != INTEL_VENDOR_ID || looks_software(&selected_props))
        LOG("WARNING: selected synthetic/non-Intel/software-like adapter: sandbox result only, NOT HD 5500 hardware validation.\n");
    LOG("SELECTED: %s vendor=0x%04X queueFamily=%u\n", selected_props.deviceName, selected_props.vendorID, queue_family);
    p_vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
    for (i = 0; i < (int)mem_props.memoryTypeCount; ++i)
        LOG("MEMORY_TYPE[%d]: heap=%u flags=0x%X\n", i, mem_props.memoryTypes[i].heapIndex, mem_props.memoryTypes[i].propertyFlags);
    {
        VkFormatProperties format_props;
        p_vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format_props);
        LOG("FORMAT RGBA8_UNORM: optimalFeatures=0x%X (need sampled=0x%X transferSrc=0x%X transferDst=0x%X)\n",
            format_props.optimalTilingFeatures, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    }
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0u, queue_family, 1u, &priority };
        VkDeviceCreateInfo create = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0u, 1u, &qci, 0u, NULL, 0u, NULL, NULL };
        const char *subset = "VK_KHR_portability_subset";
        if (device_extension_present(physical, subset)) {
            create.enabledExtensionCount = 1; create.ppEnabledExtensionNames = &subset;
            LOG("PORTABILITY: enabling advertised VK_KHR_portability_subset.\n");
        }
        VK_CALL("vkCreateDevice", p_vkCreateDevice(physical, &create, NULL, &device));
    }
    LOAD_DEVICE(device, vkDestroyDevice); LOAD_DEVICE(device, vkGetDeviceQueue);
    LOAD_DEVICE(device, vkCreateImage); LOAD_DEVICE(device, vkDestroyImage); LOAD_DEVICE(device, vkCreateImageView); LOAD_DEVICE(device, vkDestroyImageView); LOAD_DEVICE(device, vkGetImageMemoryRequirements); LOAD_DEVICE(device, vkBindImageMemory);
    LOAD_DEVICE(device, vkCreateBuffer); LOAD_DEVICE(device, vkDestroyBuffer); LOAD_DEVICE(device, vkGetBufferMemoryRequirements); LOAD_DEVICE(device, vkBindBufferMemory);
    LOAD_DEVICE(device, vkAllocateMemory); LOAD_DEVICE(device, vkFreeMemory); LOAD_DEVICE(device, vkMapMemory); LOAD_DEVICE(device, vkUnmapMemory);
    LOAD_DEVICE(device, vkFlushMappedMemoryRanges); LOAD_DEVICE(device, vkInvalidateMappedMemoryRanges);
    LOAD_DEVICE(device, vkCreateCommandPool); LOAD_DEVICE(device, vkDestroyCommandPool); LOAD_DEVICE(device, vkAllocateCommandBuffers);
    LOAD_DEVICE(device, vkBeginCommandBuffer); LOAD_DEVICE(device, vkEndCommandBuffer); LOAD_DEVICE(device, vkCmdPipelineBarrier);
    LOAD_DEVICE(device, vkCmdCopyBufferToImage); LOAD_DEVICE(device, vkCmdCopyImageToBuffer);
    LOAD_DEVICE(device, vkCreateFence); LOAD_DEVICE(device, vkDestroyFence); LOAD_DEVICE(device, vkQueueSubmit); LOAD_DEVICE(device, vkWaitForFences); LOAD_DEVICE(device, vkDeviceWaitIdle);
    NEED(vkDestroyDevice); NEED(vkGetDeviceQueue); NEED(vkCreateImage); NEED(vkDestroyImage); NEED(vkCreateImageView); NEED(vkDestroyImageView); NEED(vkGetImageMemoryRequirements); NEED(vkBindImageMemory);
    NEED(vkCreateBuffer); NEED(vkDestroyBuffer); NEED(vkGetBufferMemoryRequirements); NEED(vkBindBufferMemory); NEED(vkAllocateMemory); NEED(vkFreeMemory);
    NEED(vkMapMemory); NEED(vkUnmapMemory); NEED(vkFlushMappedMemoryRanges); NEED(vkInvalidateMappedMemoryRanges); NEED(vkCreateCommandPool); NEED(vkDestroyCommandPool);
    NEED(vkAllocateCommandBuffers); NEED(vkBeginCommandBuffer); NEED(vkEndCommandBuffer); NEED(vkCmdPipelineBarrier); NEED(vkCmdCopyBufferToImage); NEED(vkCmdCopyImageToBuffer);
    NEED(vkCreateFence); NEED(vkDestroyFence); NEED(vkQueueSubmit); NEED(vkWaitForFences); NEED(vkDeviceWaitIdle);
    p_vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (!queue) { ERR("FAIL: vkGetDeviceQueue returned VK_NULL_HANDLE.\n"); status = 7; g_first_failure = "vkGetDeviceQueue"; goto cleanup; }

    {
        VkResult r;
        r = run_sampled_image_pool_reuse(device, &mem_props);
        if (r != VK_SUCCESS) {
            status = 8;
            g_first_failure = "retained-parent sampled image/view reuse";
            goto cleanup;
        }
    }

    {
        VkImageCreateInfo info = image_info_for(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        VkMemoryAllocateInfo alloc;
        VkBufferCreateInfo upload_info, readback_info;
        VkDeviceSize upload_alloc_size, readback_alloc_size;
        const VkDeviceSize upload_copy_offset = 4;
        const VkDeviceSize readback_copy_offset = 32;
        const VkDeviceSize padded_row_bytes = 16;
        LOG("CALL: vkCreateImage(TARGET optimal RGBA8 2x2 sampled|transfer-src|transfer-dst)\n");
        VK_CALL("vkCreateImage(TARGET)", p_vkCreateImage(device, &info, NULL, &image));
        p_vkGetImageMemoryRequirements(device, image, &image_req);
        LOG("REQUIREMENTS TARGET_IMAGE: size=%llu alignment=%llu typeBits=0x%08X\n", (unsigned long long)image_req.size,
            (unsigned long long)image_req.alignment, image_req.memoryTypeBits);
        VkMemoryPropertyFlags image_required =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkDeviceSize image_bind_offset = align_up(64u, image_req.alignment);
        image_type = choose_memory_type(image_req.memoryTypeBits, &mem_props, image_required);
        if (image_type < 0) { ERR("FAIL: target image has no allocation type matching required flags=0x%X; no substitution.\n", image_required); status = 8; g_first_failure = "choose required image memory type"; goto cleanup; }
        memset(&alloc, 0, sizeof(alloc)); alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; alloc.allocationSize = image_req.size + image_bind_offset; alloc.memoryTypeIndex = (uint32_t)image_type;
        LOG("CALL: vkAllocateMemory(TARGET image requiredFlags=0x%X type=%d flags=0x%X bindOffset=%llu)\n", image_required, image_type, mem_props.memoryTypes[image_type].propertyFlags, (unsigned long long)image_bind_offset);
        VK_CALL("vkAllocateMemory(TARGET image)", p_vkAllocateMemory(device, &alloc, NULL, &image_memory));
        LOG("CALL: vkBindImageMemory(TARGET, offset=%llu)\n", (unsigned long long)image_bind_offset);
        VK_CALL("vkBindImageMemory(TARGET requested offset)", p_vkBindImageMemory(device, image, image_memory, image_bind_offset));

        memset(&upload_info, 0, sizeof(upload_info));
        upload_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; upload_info.size = 128;
        upload_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; upload_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CALL("vkCreateBuffer(upload 128B transfer-src)", p_vkCreateBuffer(device, &upload_info, NULL, &upload_buffer));
        p_vkGetBufferMemoryRequirements(device, upload_buffer, &upload_req);
        upload_bind_offset = align_up(64, upload_req.alignment);
        upload_alloc_size = upload_bind_offset + upload_req.size;
        upload_type = choose_memory_type(upload_req.memoryTypeBits, &mem_props, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        LOG("REQUIREMENTS UPLOAD: size=%llu alignment=%llu typeBits=0x%08X; bindOffset=%llu allocation=%llu\n",
            (unsigned long long)upload_req.size, (unsigned long long)upload_req.alignment, upload_req.memoryTypeBits,
            (unsigned long long)upload_bind_offset, (unsigned long long)upload_alloc_size);
        if (upload_type < 0) { ERR("FAIL: upload buffer has no HOST_VISIBLE|HOST_COHERENT type.\n"); status = 8; g_first_failure = "choose coherent upload memory type"; goto cleanup; }
        memset(&alloc, 0, sizeof(alloc)); alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; alloc.allocationSize = upload_alloc_size; alloc.memoryTypeIndex = (uint32_t)upload_type;
        LOG("CALL: vkAllocateMemory(upload host-visible coherent type=%d flags=0x%X)\n", upload_type, mem_props.memoryTypes[upload_type].propertyFlags);
        VK_CALL("vkAllocateMemory(upload)", p_vkAllocateMemory(device, &alloc, NULL, &upload_memory));
        LOG("CALL: vkBindBufferMemory(upload, nonzero memory offset=%llu)\n", (unsigned long long)upload_bind_offset);
        VK_CALL("vkBindBufferMemory(upload nonzero offset)", p_vkBindBufferMemory(device, upload_buffer, upload_memory, upload_bind_offset));
        LOG("CALL: vkMapMemory(upload allocation)\n");
        VK_CALL("vkMapMemory(upload)", p_vkMapMemory(device, upload_memory, 0, upload_alloc_size, 0, &mapped));
        mapped_memory = upload_memory;
        memset(mapped, 0xCD, (size_t)upload_alloc_size);
        memcpy((uint8_t *)mapped + upload_bind_offset + upload_copy_offset, pixels, 8);
        memcpy((uint8_t *)mapped + upload_bind_offset + upload_copy_offset + padded_row_bytes, pixels + 8, 8);
        LOG("DATA: wrote 2x2 RGBA8 at logical upload buffer offset=4 with bufferRowLength=4 (16-byte padded rows); byte lanes 8..15 are padding.\n");
        { VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, upload_memory, 0, VK_WHOLE_SIZE };
          VK_CALL("vkFlushMappedMemoryRanges(upload coherent)", p_vkFlushMappedMemoryRanges(device, 1, &range)); }
        p_vkUnmapMemory(device, upload_memory); mapped = NULL; mapped_memory = VK_NULL_HANDLE;
        LOG("RESULT: vkUnmapMemory(upload) => void\n");

        memset(&readback_info, 0, sizeof(readback_info));
        readback_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; readback_info.size = 128;
        readback_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; readback_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CALL("vkCreateBuffer(readback 128B transfer-dst)", p_vkCreateBuffer(device, &readback_info, NULL, &readback_buffer));
        p_vkGetBufferMemoryRequirements(device, readback_buffer, &readback_req);
        readback_bind_offset = align_up(64, readback_req.alignment);
        readback_alloc_size = readback_bind_offset + readback_req.size;
        readback_type = choose_memory_type(readback_req.memoryTypeBits, &mem_props, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        LOG("REQUIREMENTS READBACK: size=%llu alignment=%llu typeBits=0x%08X; bindOffset=%llu allocation=%llu\n",
            (unsigned long long)readback_req.size, (unsigned long long)readback_req.alignment, readback_req.memoryTypeBits,
            (unsigned long long)readback_bind_offset, (unsigned long long)readback_alloc_size);
        if (readback_type < 0) { ERR("FAIL: readback buffer has no HOST_VISIBLE|HOST_COHERENT type.\n"); status = 8; g_first_failure = "choose coherent readback memory type"; goto cleanup; }
        memset(&alloc, 0, sizeof(alloc)); alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; alloc.allocationSize = readback_alloc_size; alloc.memoryTypeIndex = (uint32_t)readback_type;
        LOG("CALL: vkAllocateMemory(readback host-visible coherent type=%d flags=0x%X)\n", readback_type, mem_props.memoryTypes[readback_type].propertyFlags);
        VK_CALL("vkAllocateMemory(readback)", p_vkAllocateMemory(device, &alloc, NULL, &readback_memory));
        LOG("CALL: vkBindBufferMemory(readback, nonzero memory offset=%llu)\n", (unsigned long long)readback_bind_offset);
        VK_CALL("vkBindBufferMemory(readback nonzero offset)", p_vkBindBufferMemory(device, readback_buffer, readback_memory, readback_bind_offset));

        {
            VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queue_family };
            VkCommandBufferAllocateInfo cb_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
            VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
            VkImageMemoryBarrier to_dst, to_src;
            VkBufferMemoryBarrier to_host;
            VkBufferImageCopy upload_region, readback_region;
            VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
            VkSubmitInfo submit;
            VkImageSubresourceRange range;
            memset(&range, 0, sizeof(range)); range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; range.levelCount = 1; range.layerCount = 1;
            VK_CALL("vkCreateCommandPool(transfer)", p_vkCreateCommandPool(device, &pool_info, NULL, &command_pool));
            cb_info.commandPool = command_pool;
            VK_CALL("vkAllocateCommandBuffers", p_vkAllocateCommandBuffers(device, &cb_info, &command_buffer));
            VK_CALL("vkBeginCommandBuffer(texture upload/readback)", p_vkBeginCommandBuffer(command_buffer, &begin));
            memset(&to_dst, 0, sizeof(to_dst)); to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_dst.image = image; to_dst.subresourceRange = range; to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            LOG("CALL: vkCmdPipelineBarrier(image UNDEFINED -> TRANSFER_DST_OPTIMAL)\n");
            p_vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
            LOG("RESULT: vkCmdPipelineBarrier(UNDEFINED->TRANSFER_DST) => recorded\n");
            memset(&upload_region, 0, sizeof(upload_region)); upload_region.bufferOffset = upload_copy_offset; upload_region.bufferRowLength = 4; upload_region.bufferImageHeight = 2;
            upload_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; upload_region.imageSubresource.layerCount = 1; upload_region.imageExtent.width = 2; upload_region.imageExtent.height = 2; upload_region.imageExtent.depth = 1;
            LOG("CALL: vkCmdCopyBufferToImage(upload bufferOffset=4 rowLength=4 imageExtent=2x2) [PRIMARY TARGET]\n");
            g_current_operation = "vkCmdCopyBufferToImage(upload bufferOffset=4 rowLength=4 imageExtent=2x2)";
            p_vkCmdCopyBufferToImage(command_buffer, upload_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);
            g_current_operation = NULL;
            LOG("RESULT: vkCmdCopyBufferToImage => recorded\n");
            memset(&to_src, 0, sizeof(to_src)); to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_src.image = image; to_src.subresourceRange = range;
            to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            LOG("CALL: vkCmdPipelineBarrier(image TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL)\n");
            p_vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
            LOG("RESULT: vkCmdPipelineBarrier(TRANSFER_DST->TRANSFER_SRC) => recorded\n");
            memset(&readback_region, 0, sizeof(readback_region)); readback_region.bufferOffset = readback_copy_offset;
            readback_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; readback_region.imageSubresource.layerCount = 1; readback_region.imageExtent.width = 2; readback_region.imageExtent.height = 2; readback_region.imageExtent.depth = 1;
            LOG("CALL: vkCmdCopyImageToBuffer(readback bufferOffset=32 contiguous 16 bytes)\n");
            p_vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1, &readback_region);
            LOG("RESULT: vkCmdCopyImageToBuffer => recorded\n");
            memset(&to_host, 0, sizeof(to_host)); to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_host.buffer = readback_buffer; to_host.offset = readback_copy_offset; to_host.size = 16; to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            LOG("CALL: vkCmdPipelineBarrier(readback TRANSFER_WRITE -> HOST_READ)\n");
            p_vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &to_host, 0, NULL);
            LOG("RESULT: vkCmdPipelineBarrier(readback->host) => recorded\n");
            VK_CALL("vkEndCommandBuffer(texture upload/readback)", p_vkEndCommandBuffer(command_buffer));
            VK_CALL("vkCreateFence(texture upload/readback)", p_vkCreateFence(device, &fence_info, NULL, &fence));
            memset(&submit, 0, sizeof(submit)); submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &command_buffer;
            VK_CALL("vkQueueSubmit(texture upload/readback)", p_vkQueueSubmit(queue, 1, &submit, fence));
            VK_CALL("vkWaitForFences(texture upload/readback)", p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        }
        LOG("CALL: vkMapMemory(readback allocation)\n");
        VK_CALL("vkMapMemory(readback)", p_vkMapMemory(device, readback_memory, 0, readback_alloc_size, 0, &mapped));
        mapped_memory = readback_memory;
        { VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, readback_memory, 0, VK_WHOLE_SIZE };
          VK_CALL("vkInvalidateMappedMemoryRanges(readback after fence)", p_vkInvalidateMappedMemoryRanges(device, 1, &range)); }
        LOG("READBACK 16 bytes @ logical buffer offset 32: ");
        for (i = 0; i < 16; ++i) LOG("%02X%s", ((uint8_t *)mapped)[readback_bind_offset + readback_copy_offset + i], i == 15 ? "\n" : " ");
        if (memcmp((uint8_t *)mapped + readback_bind_offset + readback_copy_offset, pixels, 16) != 0) {
            ERR("FAIL: image readback differs from uploaded RGBA8 pixels; no result is synthesized.\n"); status = 9; g_first_failure = "readback byte comparison"; goto cleanup;
        }
        p_vkUnmapMemory(device, readback_memory); mapped = NULL; mapped_memory = VK_NULL_HANDLE;
        LOG("RESULT: vkUnmapMemory(readback) => void\n");
        LOG("PASS: padded/nonzero-offset upload copied through a HOST_VISIBLE|HOST_COHERENT sampled|transfer image and exact contiguous 16-byte readback matched.\n");
    }
    {
        VkResult r = run_unused_image_parent_free_first_control(device, &mem_props);
        if (r != VK_SUCCESS) {
            status = 10;
            g_first_failure = "unused parent-free-first image control";
            goto cleanup;
        }
    }
    run_host_visible_image_preflights(device, &mem_props);
    VK_CALL("vkDeviceWaitIdle", p_vkDeviceWaitIdle(device));
    status = 0;

    /* A required preflight or driver/debug error is diagnostic failure evidence, never a pass. */
    if (g_preflight_failures) {
        ERR("FAIL: %ld required image preflight(s) returned an error.\n", g_preflight_failures);
        status = 11;
        g_first_failure = "required image preflight";
    } else if (g_debug_errors) {
        ERR("FAIL: %ld VK_EXT_debug_utils ERROR-severity callback(s) were emitted; preserving real diagnostic failure.\n", g_debug_errors);
        status = 10;
        g_first_failure = "VK_EXT_debug_utils ERROR callback";
    }

cleanup:
    if (mapped && mapped_memory && p_vkUnmapMemory) p_vkUnmapMemory(device, mapped_memory);
    if (device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(device);
    if (fence && p_vkDestroyFence) p_vkDestroyFence(device, fence, NULL);
    if (command_pool && p_vkDestroyCommandPool) p_vkDestroyCommandPool(device, command_pool, NULL);
    if (readback_buffer && p_vkDestroyBuffer) p_vkDestroyBuffer(device, readback_buffer, NULL);
    if (upload_buffer && p_vkDestroyBuffer) p_vkDestroyBuffer(device, upload_buffer, NULL);
    if (image && p_vkDestroyImage) p_vkDestroyImage(device, image, NULL);
    if (readback_memory && p_vkFreeMemory) p_vkFreeMemory(device, readback_memory, NULL);
    if (upload_memory && p_vkFreeMemory) p_vkFreeMemory(device, upload_memory, NULL);
    if (image_memory && p_vkFreeMemory) p_vkFreeMemory(device, image_memory, NULL);
    if (device && p_vkDestroyDevice) p_vkDestroyDevice(device, NULL);
    free(physical_list);
    if (debug_messenger && p_vkDestroyDebugUtilsMessengerEXT) p_vkDestroyDebugUtilsMessengerEXT(instance, debug_messenger, NULL);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    LOG("DEBUG SUMMARY: total=%ld errors=%ld preflightFailures=%ld text-integrity=%s\n", g_debug_total, g_debug_errors, g_preflight_failures,
        g_debug_suspect_text ? "SUSPECT(non-printable/unterminated callback text)" : "sane-or-no-messages");
    if (g_first_debug_error_operation) LOG("FIRST_DEBUG_ERROR_OPERATION: %s\n", g_first_debug_error_operation);
    if (status) ERR("FINAL: FAIL status=%d; FIRST_FAILURE: %s\n", status, g_first_failure ? g_first_failure : "unspecified");
    else LOG("FINAL: PASS status=0; exact raw Vulkan texture upload/readback completed.\n");
    return status;
}
