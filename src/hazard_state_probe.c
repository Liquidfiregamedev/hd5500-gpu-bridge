/*
 * HazardStateProbe: integration coverage for Ashes D3D11 v12 items 06 and 07.
 * This links no Vulkan import library.  The caller selects the loader and ICD.
 * Wine/Xvfb execution is synthetic D3D11 integration evidence, never HD 5500 evidence.
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
#include "hazard_writer_spv.h"
#include "hazard_reader_spv.h"
#include "hazard_mip_spv.h"
#include "hazard_dynamic_spv.h"
#include "hazard_rebind_spv.h"

#define INTEL_VENDOR_ID 0x8086u
#define ARRAY_LEN(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define ERR(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#define RGBA_PATTERN 0x44332211u
#define RGBA_ALT 0xD4C3B2A1u
#define RGBA_ALT2 0x11223344u

typedef struct Probe {
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
} Probe;

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
static PFN_vkCreateImage p_vkCreateImage;
static PFN_vkDestroyImage p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindBufferMemory p_vkBindBufferMemory;
static PFN_vkBindImageMemory p_vkBindImageMemory;
static PFN_vkMapMemory p_vkMapMemory;
static PFN_vkUnmapMemory p_vkUnmapMemory;
static PFN_vkFlushMappedMemoryRanges p_vkFlushMappedMemoryRanges;
static PFN_vkInvalidateMappedMemoryRanges p_vkInvalidateMappedMemoryRanges;
static PFN_vkCreateImageView p_vkCreateImageView;
static PFN_vkDestroyImageView p_vkDestroyImageView;
static PFN_vkCreateSampler p_vkCreateSampler;
static PFN_vkDestroySampler p_vkDestroySampler;
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
static PFN_vkResetCommandBuffer p_vkResetCommandBuffer;
static PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
static PFN_vkCmdBindPipeline p_vkCmdBindPipeline;
static PFN_vkCmdBindDescriptorSets p_vkCmdBindDescriptorSets;
static PFN_vkCmdDispatch p_vkCmdDispatch;
static PFN_vkCmdUpdateBuffer p_vkCmdUpdateBuffer;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkDestroyFence p_vkDestroyFence;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkWaitForFences p_vkWaitForFences;
static PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle;

#define LOAD_GLOBAL(n) p_##n = (PFN_##n)gipa(VK_NULL_HANDLE, #n)
#define LOAD_INSTANCE(i,n) p_##n = (PFN_##n)gipa((i), #n)
#define LOAD_DEVICE(d,n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr((d), #n)
#define NEED(n) do { if (!p_##n) { ERR("FAIL: required Vulkan entry point unavailable: %s\n", #n); return 0; } } while (0)

static int g_informational;
static int g_failures;

static const char *result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
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
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default: return "unrecognized VkResult";
    }
}

static int vk_ok(const char *what, VkResult r) {
    LOG("RESULT: %s => %s (%d)\n", what, result_name(r), (int)r);
    return r == VK_SUCCESS;
}

static void record_case(const char *name, int pass, const char *detail) {
    if (pass) {
        LOG("CASE PASS: %s %s\n", name, detail);
    } else if (g_informational) {
        LOG("CASE INFO: %s %s\n", name, detail);
    } else {
        LOG("CASE FAIL: %s %s\n", name, detail);
        ++g_failures;
    }
}

static void mismatch_u32(const char *name, uint32_t index, uint32_t expected, uint32_t actual) {
    LOG("%s: %s index=%u expected=0x%08X actual=0x%08X\n",
        g_informational ? "INFO-MISMATCH" : "MISMATCH", name, index, expected, actual);
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

static int find_compute_queue(VkPhysicalDevice physical, uint32_t *out) {
    uint32_t count = 0, i;
    VkQueueFamilyProperties *q;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (!count) return 0;
    q = (VkQueueFamilyProperties *)calloc(count, sizeof(*q));
    if (!q) return 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, q);
    for (i = 0; i < count; ++i) {
        LOG("QUEUE[%u]: flags=0x%X count=%u\n", i, (unsigned)q[i].queueFlags, q[i].queueCount);
        if (q[i].queueCount && (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            *out = i; free(q); return 1;
        }
    }
    free(q);
    return 0;
}

static int choose_memory_type(uint32_t bits, VkMemoryPropertyFlags required,
    const VkPhysicalDeviceMemoryProperties *mp) {
    uint32_t i;
    for (i = 0; i < mp->memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & required) == required) return (int)i;
    return -1;
}

static int create_buffer(Probe *p, VkDeviceSize size, VkBufferUsageFlags usage,
    VkBuffer *buffer, VkDeviceMemory *memory, int *coherent) {
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkResult r;
    int type;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = p_vkCreateBuffer(p->device, &bi, NULL, buffer);
    if (!vk_ok("vkCreateBuffer", r)) return 0;
    p_vkGetBufferMemoryRequirements(p->device, *buffer, &req);
    type = choose_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &p->memory);
    if (type < 0) type = choose_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &p->memory);
    if (type < 0) { ERR("FAIL: no host-visible memory type for buffer.\n"); return 0; }
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)type;
    r = p_vkAllocateMemory(p->device, &ai, NULL, memory);
    if (!vk_ok("vkAllocateMemory(buffer)", r)) return 0;
    r = p_vkBindBufferMemory(p->device, *buffer, *memory, 0);
    if (!vk_ok("vkBindBufferMemory", r)) return 0;
    *coherent = (p->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return 1;
}

static int create_image(Probe *p, uint32_t width, uint32_t height, uint32_t mips,
    VkImage *image, VkDeviceMemory *memory) {
    VkImageCreateInfo ii;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkResult r;
    int type;
    memset(&ii, 0, sizeof(ii)); ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UINT;
    ii.extent.width = width; ii.extent.height = height; ii.extent.depth = 1u;
    ii.mipLevels = mips; ii.arrayLayers = 1u; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = p_vkCreateImage(p->device, &ii, NULL, image);
    if (!vk_ok("vkCreateImage(RGBA8)", r)) return 0;
    p_vkGetImageMemoryRequirements(p->device, *image, &req);
    type = choose_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &p->memory);
    if (type < 0) type = choose_memory_type(req.memoryTypeBits, 0u, &p->memory);
    if (type < 0) { ERR("FAIL: no compatible image memory type.\n"); return 0; }
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)type;
    r = p_vkAllocateMemory(p->device, &ai, NULL, memory);
    if (!vk_ok("vkAllocateMemory(image)", r)) return 0;
    r = p_vkBindImageMemory(p->device, *image, *memory, 0);
    return vk_ok("vkBindImageMemory", r);
}

static int create_view(Probe *p, VkImage image, uint32_t base_mip, uint32_t levels, VkImageView *view) {
    VkImageViewCreateInfo vi;
    VkResult r;
    memset(&vi, 0, sizeof(vi)); vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UINT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = base_mip; vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.baseArrayLayer = 0; vi.subresourceRange.layerCount = 1;
    r = p_vkCreateImageView(p->device, &vi, NULL, view);
    return vk_ok("vkCreateImageView", r);
}

static int create_sampler(Probe *p, VkSampler *sampler) {
    VkSamplerCreateInfo si;
    VkResult r;
    memset(&si, 0, sizeof(si)); si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    r = p_vkCreateSampler(p->device, &si, NULL, sampler);
    return vk_ok("vkCreateSampler", r);
}

static int create_set_layout(Probe *p, const VkDescriptorSetLayoutBinding *bindings,
    uint32_t count, VkDescriptorSetLayout *layout) {
    VkDescriptorSetLayoutCreateInfo ci;
    VkResult r;
    memset(&ci, 0, sizeof(ci)); ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = count; ci.pBindings = bindings;
    r = p_vkCreateDescriptorSetLayout(p->device, &ci, NULL, layout);
    return vk_ok("vkCreateDescriptorSetLayout", r);
}

static int create_pool(Probe *p, const VkDescriptorPoolSize *sizes, uint32_t size_count,
    uint32_t max_sets, VkDescriptorPool *pool) {
    VkDescriptorPoolCreateInfo ci;
    VkResult r;
    memset(&ci, 0, sizeof(ci)); ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = max_sets; ci.poolSizeCount = size_count; ci.pPoolSizes = sizes;
    r = p_vkCreateDescriptorPool(p->device, &ci, NULL, pool);
    return vk_ok("vkCreateDescriptorPool", r);
}

static int allocate_set(Probe *p, VkDescriptorPool pool, VkDescriptorSetLayout layout, VkDescriptorSet *set) {
    VkDescriptorSetAllocateInfo ai;
    VkResult r;
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
    r = p_vkAllocateDescriptorSets(p->device, &ai, set);
    return vk_ok("vkAllocateDescriptorSets", r);
}

static int create_pipeline(Probe *p, VkDescriptorSetLayout set_layout,
    const uint32_t *code, size_t code_size, VkPipelineLayout *layout, VkPipeline *pipeline) {
    VkPipelineLayoutCreateInfo li;
    VkShaderModuleCreateInfo mi;
    VkComputePipelineCreateInfo ci;
    VkPipelineShaderStageCreateInfo stage;
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult r;
    memset(&li, 0, sizeof(li)); li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1; li.pSetLayouts = &set_layout;
    r = p_vkCreatePipelineLayout(p->device, &li, NULL, layout);
    if (!vk_ok("vkCreatePipelineLayout", r)) return 0;
    memset(&mi, 0, sizeof(mi)); mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = code_size; mi.pCode = code;
    r = p_vkCreateShaderModule(p->device, &mi, NULL, &module);
    if (!vk_ok("vkCreateShaderModule", r)) return 0;
    memset(&stage, 0, sizeof(stage)); stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = module; stage.pName = "main";
    memset(&ci, 0, sizeof(ci)); ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage; ci.layout = *layout;
    r = p_vkCreateComputePipelines(p->device, VK_NULL_HANDLE, 1u, &ci, NULL, pipeline);
    p_vkDestroyShaderModule(p->device, module, NULL);
    return vk_ok("vkCreateComputePipelines", r);
}

static int begin_commands(Probe *p, VkCommandPool *pool, VkCommandBuffer *cmd) {
    VkCommandPoolCreateInfo pi;
    VkCommandBufferAllocateInfo ai;
    VkCommandBufferBeginInfo bi;
    VkResult r;
    memset(&pi, 0, sizeof(pi)); pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = p->family;
    r = p_vkCreateCommandPool(p->device, &pi, NULL, pool);
    if (!vk_ok("vkCreateCommandPool", r)) return 0;
    memset(&ai, 0, sizeof(ai)); ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = *pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    r = p_vkAllocateCommandBuffers(p->device, &ai, cmd);
    if (!vk_ok("vkAllocateCommandBuffers", r)) return 0;
    memset(&bi, 0, sizeof(bi)); bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = p_vkBeginCommandBuffer(*cmd, &bi);
    return vk_ok("vkBeginCommandBuffer", r);
}

static int submit_wait(Probe *p, VkCommandBuffer cmd) {
    VkFenceCreateInfo fi;
    VkSubmitInfo si;
    VkFence fence = VK_NULL_HANDLE;
    VkResult r;
    memset(&fi, 0, sizeof(fi)); fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    r = p_vkCreateFence(p->device, &fi, NULL, &fence);
    if (!vk_ok("vkCreateFence", r)) return 0;
    memset(&si, 0, sizeof(si)); si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    r = p_vkQueueSubmit(p->queue, 1u, &si, fence);
    if (!vk_ok("vkQueueSubmit", r)) { p_vkDestroyFence(p->device, fence, NULL); return 0; }
    r = p_vkWaitForFences(p->device, 1u, &fence, VK_TRUE, UINT64_C(20000000000));
    p_vkDestroyFence(p->device, fence, NULL);
    return vk_ok("vkWaitForFences(timeout=20s)", r);
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, uint32_t base_mip, uint32_t levels,
    VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b)); b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = old_layout; b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = base_mip; b.subresourceRange.levelCount = levels;
    b.subresourceRange.baseArrayLayer = 0; b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
    p_vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0u, 0u, NULL, 0u, NULL, 1u, &b);
}

static void buffer_to_uniform_barrier(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize size) {
    VkBufferMemoryBarrier b;
    memset(&b, 0, sizeof(b)); b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    b.buffer = buffer; b.offset = 0; b.size = size;
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, NULL, 1u, &b, 0u, NULL);
}

static void buffer_to_host_barrier(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize size) {
    VkBufferMemoryBarrier b;
    memset(&b, 0, sizeof(b)); b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.buffer = buffer; b.offset = 0; b.size = size;
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0u, 0u, NULL, 1u, &b, 0u, NULL);
}

static int read_u32s(Probe *p, VkDeviceMemory memory, VkDeviceSize bytes, int coherent, uint32_t *dst) {
    void *mapped = NULL;
    VkResult r = p_vkMapMemory(p->device, memory, 0, bytes, 0u, &mapped);
    if (!vk_ok("vkMapMemory(readback)", r) || !mapped) return 0;
    if (!coherent) {
        VkMappedMemoryRange range;
        memset(&range, 0, sizeof(range)); range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = memory; range.offset = 0; range.size = bytes;
        r = p_vkInvalidateMappedMemoryRanges(p->device, 1u, &range);
        if (!vk_ok("vkInvalidateMappedMemoryRanges", r)) { p_vkUnmapMemory(p->device, memory); return 0; }
    }
    memcpy(dst, mapped, (size_t)bytes);
    p_vkUnmapMemory(p->device, memory);
    return 1;
}

/* A: writer uses storage-image binding 7 (D3D11 u7). Reader uses sampled binding 5
 * (t5) and output storage buffer binding 1 (u1), deliberately not u7. */
static int case_stale_uav_to_srv(Probe *p) {
    VkImage image = VK_NULL_HANDLE; VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkImageView storage_view = VK_NULL_HANDLE, sampled_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkBuffer output = VK_NULL_HANDLE; VkDeviceMemory output_mem = VK_NULL_HANDLE;
    VkDescriptorSetLayout writer_sl = VK_NULL_HANDLE, reader_sl = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE; VkDescriptorSet writer_set = VK_NULL_HANDLE, reader_set = VK_NULL_HANDLE;
    VkPipelineLayout writer_pl = VK_NULL_HANDLE, reader_pl = VK_NULL_HANDLE;
    VkPipeline writer_pipe = VK_NULL_HANDLE, reader_pipe = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding wb, rb[2]; VkDescriptorPoolSize ps[3];
    VkDescriptorImageInfo storage_info, sample_info; VkDescriptorBufferInfo output_info;
    VkWriteDescriptorSet writes[3]; uint32_t values[64]; int coherent = 0, ok = 0; uint32_t i;

    LOG("CASE_BEGIN: stale-uav-to-srv writer=u7 reader=t5 output=u1 same RGBA8 mip0\n");
    if (!create_image(p, 8, 8, 1, &image, &image_mem) || !create_view(p, image, 0, 1, &storage_view) ||
        !create_view(p, image, 0, 1, &sampled_view) || !create_sampler(p, &sampler) ||
        !create_buffer(p, sizeof(values), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &output, &output_mem, &coherent)) goto done;
    memset(&wb, 0, sizeof(wb)); wb.binding = 7; wb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wb.descriptorCount = 1; wb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    memset(rb, 0, sizeof(rb)); rb[0].binding = 1; rb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; rb[0].descriptorCount = 1; rb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    rb[1].binding = 5; rb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rb[1].descriptorCount = 1; rb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    if (!create_set_layout(p, &wb, 1, &writer_sl) || !create_set_layout(p, rb, 2, &reader_sl) ||
        !create_pipeline(p, writer_sl, hazard_writer_spv, hazard_writer_spv_size, &writer_pl, &writer_pipe) ||
        !create_pipeline(p, reader_sl, hazard_reader_spv, hazard_reader_spv_size, &reader_pl, &reader_pipe)) goto done;
    ps[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; ps[0].descriptorCount = 1;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[1].descriptorCount = 1;
    ps[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[2].descriptorCount = 1;
    if (!create_pool(p, ps, 3, 2, &pool) || !allocate_set(p, pool, writer_sl, &writer_set) || !allocate_set(p, pool, reader_sl, &reader_set)) goto done;
    memset(&storage_info, 0, sizeof(storage_info)); storage_info.imageView = storage_view; storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    memset(&sample_info, 0, sizeof(sample_info)); sample_info.sampler = sampler; sample_info.imageView = sampled_view; sample_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    output_info.buffer = output; output_info.offset = 0; output_info.range = VK_WHOLE_SIZE;
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = writer_set; writes[0].dstBinding = 7; writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[0].pImageInfo = &storage_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = reader_set; writes[1].dstBinding = 5; writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &sample_info;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[2].dstSet = reader_set; writes[2].dstBinding = 1; writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &output_info;
    p_vkUpdateDescriptorSets(p->device, 3, writes, 0, NULL);
    if (!begin_commands(p, &cmd_pool, &cmd)) goto done;
    image_barrier(cmd, image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0u, VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, writer_pipe);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, writer_pl, 0, 1, &writer_set, 0, NULL);
    p_vkCmdDispatch(cmd, 1, 1, 1);
    image_barrier(cmd, image, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reader_pipe);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reader_pl, 0, 1, &reader_set, 0, NULL);
    p_vkCmdDispatch(cmd, 1, 1, 1);
    buffer_to_host_barrier(cmd, output, sizeof(values));
    if (!vk_ok("vkEndCommandBuffer(case A)", p_vkEndCommandBuffer(cmd)) || !submit_wait(p, cmd) ||
        !read_u32s(p, output_mem, sizeof(values), coherent, values)) goto done;
    ok = 1;
    for (i = 0; i < ARRAY_LEN(values); ++i) if (values[i] != RGBA_PATTERN) { mismatch_u32("stale-uav-to-srv", i, RGBA_PATTERN, values[i]); ok = 0; }
done:
    record_case("stale-uav-to-srv", ok, "writer u7 remained bound while reader binds same image t5 and output u1");
    if (cmd_pool) p_vkDestroyCommandPool(p->device, cmd_pool, NULL);
    if (writer_pipe) p_vkDestroyPipeline(p->device, writer_pipe, NULL);
    if (reader_pipe) p_vkDestroyPipeline(p->device, reader_pipe, NULL);
    if (writer_pl) p_vkDestroyPipelineLayout(p->device, writer_pl, NULL);
    if (reader_pl) p_vkDestroyPipelineLayout(p->device, reader_pl, NULL);
    if (pool) p_vkDestroyDescriptorPool(p->device, pool, NULL);
    if (writer_sl) p_vkDestroyDescriptorSetLayout(p->device, writer_sl, NULL);
    if (reader_sl) p_vkDestroyDescriptorSetLayout(p->device, reader_sl, NULL);
    if (sampler) p_vkDestroySampler(p->device, sampler, NULL);
    if (storage_view) p_vkDestroyImageView(p->device, storage_view, NULL);
    if (sampled_view) p_vkDestroyImageView(p->device, sampled_view, NULL);
    if (output) p_vkDestroyBuffer(p->device, output, NULL);
    if (output_mem) p_vkFreeMemory(p->device, output_mem, NULL);
    if (image) p_vkDestroyImage(p->device, image, NULL);
    if (image_mem) p_vkFreeMemory(p->device, image_mem, NULL);
    return ok;
}

/* B: mip0 remains an SRV while a non-overlapping mip1 view is written as u11. */
static int case_disjoint_mips(Probe *p) {
    VkImage image = VK_NULL_HANDLE; VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkImageView mip0_storage = VK_NULL_HANDLE, mip0_sample = VK_NULL_HANDLE, mip1_storage = VK_NULL_HANDLE, mip1_sample = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE; VkBuffer output = VK_NULL_HANDLE; VkDeviceMemory output_mem = VK_NULL_HANDLE;
    VkDescriptorSetLayout writer_sl = VK_NULL_HANDLE, mip_sl = VK_NULL_HANDLE, reader_sl = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE; VkDescriptorSet writer_set = VK_NULL_HANDLE, mip_set = VK_NULL_HANDLE, reader_set = VK_NULL_HANDLE;
    VkPipelineLayout writer_pl = VK_NULL_HANDLE, mip_pl = VK_NULL_HANDLE, reader_pl = VK_NULL_HANDLE;
    VkPipeline writer_pipe = VK_NULL_HANDLE, mip_pipe = VK_NULL_HANDLE, reader_pipe = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding wb, mb[2], rb[2]; VkDescriptorPoolSize ps[3];
    VkDescriptorImageInfo writer_info, mip0_info, mip1_info; VkDescriptorBufferInfo output_info;
    VkWriteDescriptorSet writes[5]; uint32_t values[64]; int coherent = 0, ok = 0; uint32_t i;

    LOG("CASE_BEGIN: disjoint-subresources sampled mip0=t3 and storage mip1=u11 of one image\n");
    if (!create_image(p, 8, 8, 2, &image, &image_mem) || !create_view(p, image, 0, 1, &mip0_storage) ||
        !create_view(p, image, 0, 1, &mip0_sample) || !create_view(p, image, 1, 1, &mip1_storage) ||
        !create_view(p, image, 1, 1, &mip1_sample) || !create_sampler(p, &sampler) ||
        !create_buffer(p, sizeof(values), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &output, &output_mem, &coherent)) goto done;
    memset(&wb, 0, sizeof(wb)); wb.binding = 7; wb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wb.descriptorCount = 1; wb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    memset(mb, 0, sizeof(mb)); mb[0].binding = 3; mb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; mb[0].descriptorCount = 1; mb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    mb[1].binding = 11; mb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; mb[1].descriptorCount = 1; mb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    memset(rb, 0, sizeof(rb)); rb[0].binding = 1; rb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; rb[0].descriptorCount = 1; rb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    rb[1].binding = 5; rb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rb[1].descriptorCount = 1; rb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    if (!create_set_layout(p, &wb, 1, &writer_sl) || !create_set_layout(p, mb, 2, &mip_sl) || !create_set_layout(p, rb, 2, &reader_sl) ||
        !create_pipeline(p, writer_sl, hazard_writer_spv, hazard_writer_spv_size, &writer_pl, &writer_pipe) ||
        !create_pipeline(p, mip_sl, hazard_mip_spv, hazard_mip_spv_size, &mip_pl, &mip_pipe) ||
        !create_pipeline(p, reader_sl, hazard_reader_spv, hazard_reader_spv_size, &reader_pl, &reader_pipe)) goto done;
    ps[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; ps[0].descriptorCount = 2; ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[1].descriptorCount = 2; ps[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[2].descriptorCount = 1;
    if (!create_pool(p, ps, 3, 3, &pool) || !allocate_set(p, pool, writer_sl, &writer_set) || !allocate_set(p, pool, mip_sl, &mip_set) || !allocate_set(p, pool, reader_sl, &reader_set)) goto done;
    memset(&writer_info, 0, sizeof(writer_info)); writer_info.imageView = mip0_storage; writer_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    memset(&mip0_info, 0, sizeof(mip0_info)); mip0_info.sampler = sampler; mip0_info.imageView = mip0_sample; mip0_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    memset(&mip1_info, 0, sizeof(mip1_info)); mip1_info.sampler = sampler; mip1_info.imageView = mip1_sample; mip1_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    output_info.buffer = output; output_info.offset = 0; output_info.range = VK_WHOLE_SIZE;
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = writer_set; writes[0].dstBinding = 7; writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[0].pImageInfo = &writer_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = mip_set; writes[1].dstBinding = 3; writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &mip0_info;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[2].dstSet = mip_set; writes[2].dstBinding = 11; writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[2].pImageInfo = &writer_info;
    writes[2].pImageInfo = &writer_info; writes[2].pImageInfo = &writer_info;
    /* replace image-view field with mip1 storage while retaining general layout */
    { static VkDescriptorImageInfo mip1_storage_info; mip1_storage_info.sampler = VK_NULL_HANDLE; mip1_storage_info.imageView = mip1_storage; mip1_storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL; writes[2].pImageInfo = &mip1_storage_info; }
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[3].dstSet = reader_set; writes[3].dstBinding = 5; writes[3].descriptorCount = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].pImageInfo = &mip1_info;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[4].dstSet = reader_set; writes[4].dstBinding = 1; writes[4].descriptorCount = 1; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[4].pBufferInfo = &output_info;
    p_vkUpdateDescriptorSets(p->device, 5, writes, 0, NULL);
    if (!begin_commands(p, &cmd_pool, &cmd)) goto done;
    image_barrier(cmd, image, 0, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0u, VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, writer_pipe); p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, writer_pl, 0, 1, &writer_set, 0, NULL); p_vkCmdDispatch(cmd, 1, 1, 1);
    image_barrier(cmd, image, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mip_pipe); p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mip_pl, 0, 1, &mip_set, 0, NULL); p_vkCmdDispatch(cmd, 1, 1, 1);
    image_barrier(cmd, image, 1, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reader_pipe); p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reader_pl, 0, 1, &reader_set, 0, NULL); p_vkCmdDispatch(cmd, 1, 1, 1);
    buffer_to_host_barrier(cmd, output, sizeof(values));
    if (!vk_ok("vkEndCommandBuffer(case B)", p_vkEndCommandBuffer(cmd)) || !submit_wait(p, cmd) || !read_u32s(p, output_mem, sizeof(values), coherent, values)) goto done;
    ok = 1; for (i = 0; i < ARRAY_LEN(values); ++i) if (values[i] != RGBA_PATTERN) { mismatch_u32("disjoint-subresources", i, RGBA_PATTERN, values[i]); ok = 0; }
done:
    record_case("disjoint-subresources", ok, "mip0 SRV and mip1 UAV of one image produced exact copied texels");
    if (cmd_pool) p_vkDestroyCommandPool(p->device, cmd_pool, NULL);
    if (writer_pipe) p_vkDestroyPipeline(p->device, writer_pipe, NULL);
    if (mip_pipe) p_vkDestroyPipeline(p->device, mip_pipe, NULL);
    if (reader_pipe) p_vkDestroyPipeline(p->device, reader_pipe, NULL);
    if (writer_pl) p_vkDestroyPipelineLayout(p->device, writer_pl, NULL);
    if (mip_pl) p_vkDestroyPipelineLayout(p->device, mip_pl, NULL);
    if (reader_pl) p_vkDestroyPipelineLayout(p->device, reader_pl, NULL);
    if (pool) p_vkDestroyDescriptorPool(p->device, pool, NULL);
    if (writer_sl) p_vkDestroyDescriptorSetLayout(p->device, writer_sl, NULL);
    if (mip_sl) p_vkDestroyDescriptorSetLayout(p->device, mip_sl, NULL);
    if (reader_sl) p_vkDestroyDescriptorSetLayout(p->device, reader_sl, NULL);
    if (sampler) p_vkDestroySampler(p->device, sampler, NULL);
    if (mip0_storage) p_vkDestroyImageView(p->device, mip0_storage, NULL);
    if (mip0_sample) p_vkDestroyImageView(p->device, mip0_sample, NULL);
    if (mip1_storage) p_vkDestroyImageView(p->device, mip1_storage, NULL);
    if (mip1_sample) p_vkDestroyImageView(p->device, mip1_sample, NULL);
    if (output) p_vkDestroyBuffer(p->device, output, NULL);
    if (output_mem) p_vkFreeMemory(p->device, output_mem, NULL);
    if (image) p_vkDestroyImage(p->device, image, NULL);
    if (image_mem) p_vkFreeMemory(p->device, image_mem, NULL);
    return ok;
}

static int case_dynamic_offsets(Probe *p) {
    VkBuffer uniform = VK_NULL_HANDLE, output = VK_NULL_HANDLE; VkDeviceMemory uniform_mem = VK_NULL_HANDLE, output_mem = VK_NULL_HANDLE;
    VkDescriptorSetLayout sl = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE; VkCommandPool cmd_pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding bindings[2]; VkDescriptorPoolSize ps[2]; VkDescriptorBufferInfo ub, ob; VkWriteDescriptorSet writes[2];
    uint8_t uniform_bytes[512]; uint32_t result[2]; uint32_t offsets[1]; int uc = 0, oc = 0, ok = 0;
    LOG("CASE_BEGIN: dynamic-uniform-offsets same set binding b0 offsets 0 and 256 output u4\n");
    memset(uniform_bytes, 0, sizeof(uniform_bytes)); ((uint32_t *)uniform_bytes)[0] = 0x13579BDFu; ((uint32_t *)uniform_bytes)[1] = 0u; ((uint32_t *)(uniform_bytes + 256))[0] = 0x2468ACE0u; ((uint32_t *)(uniform_bytes + 256))[1] = 1u;
    if (!create_buffer(p, sizeof(uniform_bytes), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &uniform, &uniform_mem, &uc) ||
        !create_buffer(p, sizeof(result), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &output, &output_mem, &oc)) goto done;
    memset(bindings, 0, sizeof(bindings)); bindings[0].binding = 0; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; bindings[0].descriptorCount = 1; bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 4; bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[1].descriptorCount = 1; bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    if (!create_set_layout(p, bindings, 2, &sl) || !create_pipeline(p, sl, hazard_dynamic_spv, hazard_dynamic_spv_size, &pl, &pipe)) goto done;
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; ps[0].descriptorCount = 1; ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[1].descriptorCount = 1;
    if (!create_pool(p, ps, 2, 1, &pool) || !allocate_set(p, pool, sl, &set)) goto done;
    ub.buffer = uniform; ub.offset = 0; ub.range = 16; ob.buffer = output; ob.offset = 0; ob.range = VK_WHOLE_SIZE;
    memset(writes, 0, sizeof(writes)); writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = set; writes[0].dstBinding = 0; writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; writes[0].pBufferInfo = &ub;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = set; writes[1].dstBinding = 4; writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &ob;
    p_vkUpdateDescriptorSets(p->device, 2, writes, 0, NULL);
    if (!begin_commands(p, &cmd_pool, &cmd)) goto done;
    p_vkCmdUpdateBuffer(cmd, uniform, 0, sizeof(uniform_bytes), uniform_bytes);
    buffer_to_uniform_barrier(cmd, uniform, sizeof(uniform_bytes));
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); offsets[0] = 0; p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 1, offsets); p_vkCmdDispatch(cmd, 1, 1, 1);
    offsets[0] = 256; p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 1, offsets); p_vkCmdDispatch(cmd, 1, 1, 1);
    buffer_to_host_barrier(cmd, output, sizeof(result));
    if (!vk_ok("vkEndCommandBuffer(case C)", p_vkEndCommandBuffer(cmd)) || !submit_wait(p, cmd) || !read_u32s(p, output_mem, sizeof(result), oc, result)) goto done;
    ok = result[0] == 0x13579BDFu && result[1] == 0x2468ACE0u;
    if (result[0] != 0x13579BDFu) mismatch_u32("dynamic-uniform-offsets", 0, 0x13579BDFu, result[0]);
    if (result[1] != 0x2468ACE0u) mismatch_u32("dynamic-uniform-offsets", 1, 0x2468ACE0u, result[1]);
done:
    record_case("dynamic-uniform-offsets", ok, "same descriptor set bound with dynamic offsets 0 then 256");
    if (cmd_pool) p_vkDestroyCommandPool(p->device, cmd_pool, NULL);
    if (pipe) p_vkDestroyPipeline(p->device, pipe, NULL);
    if (pl) p_vkDestroyPipelineLayout(p->device, pl, NULL);
    if (pool) p_vkDestroyDescriptorPool(p->device, pool, NULL);
    if (sl) p_vkDestroyDescriptorSetLayout(p->device, sl, NULL);
    if (uniform) p_vkDestroyBuffer(p->device, uniform, NULL);
    if (output) p_vkDestroyBuffer(p->device, output, NULL);
    if (uniform_mem) p_vkFreeMemory(p->device, uniform_mem, NULL);
    if (output_mem) p_vkFreeMemory(p->device, output_mem, NULL);
    return ok;
}

static int case_rebind_and_update(Probe *p) {
    VkBuffer in_a = VK_NULL_HANDLE, in_b = VK_NULL_HANDLE, out_a = VK_NULL_HANDLE, out_b = VK_NULL_HANDLE, out_c = VK_NULL_HANDLE;
    VkDeviceMemory mia = VK_NULL_HANDLE, mib = VK_NULL_HANDLE, moa = VK_NULL_HANDLE, mob = VK_NULL_HANDLE, moc = VK_NULL_HANDLE;
    VkDescriptorSetLayout sl = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set_a = VK_NULL_HANDLE, set_b = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding bindings[2]; VkDescriptorPoolSize ps[2];
    VkDescriptorBufferInfo ia, ib, oa, ob, oc; VkWriteDescriptorSet wa[2], wb[2];
    VkCommandBufferBeginInfo begin; uint32_t input_a = 0x0A1B2C3Du, input_b = 0x4E5F6071u;
    uint32_t va = 0, vb = 0, vc = 0; int cia = 0, cib = 0, coa = 0, cob = 0, coc = 0, ok = 0;
    LOG("CASE_BEGIN: descriptor-rebind same layout setA=UBO-A/outA setB=UBO-B/outB then vkUpdateDescriptorSets setA=UBO-B/outC and re-record\n");
    if (!create_buffer(p, sizeof(input_a), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &in_a, &mia, &cia) ||
        !create_buffer(p, sizeof(input_b), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &in_b, &mib, &cib) ||
        !create_buffer(p, sizeof(va), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out_a, &moa, &coa) ||
        !create_buffer(p, sizeof(vb), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out_b, &mob, &cob) ||
        !create_buffer(p, sizeof(vc), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out_c, &moc, &coc)) goto done;
    memset(bindings, 0, sizeof(bindings));
    bindings[0].binding = 2; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; bindings[0].descriptorCount = 1; bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 6; bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[1].descriptorCount = 1; bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    if (!create_set_layout(p, bindings, 2, &sl) || !create_pipeline(p, sl, hazard_rebind_spv, hazard_rebind_spv_size, &pl, &pipe)) goto done;
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ps[0].descriptorCount = 2; ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[1].descriptorCount = 2;
    if (!create_pool(p, ps, 2, 2, &pool) || !allocate_set(p, pool, sl, &set_a) || !allocate_set(p, pool, sl, &set_b)) goto done;
    ia.buffer = in_a; ia.offset = 0; ia.range = VK_WHOLE_SIZE; ib.buffer = in_b; ib.offset = 0; ib.range = VK_WHOLE_SIZE;
    oa.buffer = out_a; oa.offset = 0; oa.range = VK_WHOLE_SIZE; ob.buffer = out_b; ob.offset = 0; ob.range = VK_WHOLE_SIZE; oc.buffer = out_c; oc.offset = 0; oc.range = VK_WHOLE_SIZE;
    memset(wa, 0, sizeof(wa));
    wa[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wa[0].dstSet = set_a; wa[0].dstBinding = 2; wa[0].descriptorCount = 1; wa[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wa[0].pBufferInfo = &ia;
    wa[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wa[1].dstSet = set_a; wa[1].dstBinding = 6; wa[1].descriptorCount = 1; wa[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wa[1].pBufferInfo = &oa;
    memset(wb, 0, sizeof(wb));
    wb[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wb[0].dstSet = set_b; wb[0].dstBinding = 2; wb[0].descriptorCount = 1; wb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wb[0].pBufferInfo = &ib;
    wb[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wb[1].dstSet = set_b; wb[1].dstBinding = 6; wb[1].descriptorCount = 1; wb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wb[1].pBufferInfo = &ob;
    p_vkUpdateDescriptorSets(p->device, 2, wa, 0, NULL); p_vkUpdateDescriptorSets(p->device, 2, wb, 0, NULL);
    if (!begin_commands(p, &cmd_pool, &cmd)) goto done;
    p_vkCmdUpdateBuffer(cmd, in_a, 0, sizeof(input_a), &input_a); p_vkCmdUpdateBuffer(cmd, in_b, 0, sizeof(input_b), &input_b);
    buffer_to_uniform_barrier(cmd, in_a, sizeof(input_a)); buffer_to_uniform_barrier(cmd, in_b, sizeof(input_b));
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set_a, 0, NULL); p_vkCmdDispatch(cmd, 1, 1, 1);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set_b, 0, NULL); p_vkCmdDispatch(cmd, 1, 1, 1);
    buffer_to_host_barrier(cmd, out_a, sizeof(va)); buffer_to_host_barrier(cmd, out_b, sizeof(vb));
    if (!vk_ok("vkEndCommandBuffer(case D first)", p_vkEndCommandBuffer(cmd)) || !submit_wait(p, cmd) ||
        !read_u32s(p, moa, sizeof(va), coa, &va) || !read_u32s(p, mob, sizeof(vb), cob, &vb)) goto done;
    if (va != input_a) mismatch_u32("descriptor-rebind-first", 0, input_a, va);
    if (vb != input_b) mismatch_u32("descriptor-rebind-first", 1, input_b, vb);
    if (va != input_a || vb != input_b) goto done;
    /* Same descriptor-set handle now points to a different UBO and output buffer. */
    wa[0].pBufferInfo = &ib; wa[1].pBufferInfo = &oc; p_vkUpdateDescriptorSets(p->device, 2, wa, 0, NULL);
    if (!vk_ok("vkResetCommandBuffer(case D re-record)", p_vkResetCommandBuffer(cmd, 0u))) goto done;
    memset(&begin, 0, sizeof(begin)); begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vk_ok("vkBeginCommandBuffer(case D re-record)", p_vkBeginCommandBuffer(cmd, &begin))) goto done;
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set_a, 0, NULL); p_vkCmdDispatch(cmd, 1, 1, 1);
    buffer_to_host_barrier(cmd, out_c, sizeof(vc));
    if (!vk_ok("vkEndCommandBuffer(case D re-record)", p_vkEndCommandBuffer(cmd)) || !submit_wait(p, cmd) || !read_u32s(p, moc, sizeof(vc), coc, &vc)) goto done;
    if (vc != input_b) { mismatch_u32("descriptor-update-re-record", 2, input_b, vc); goto done; }
    ok = 1;
done:
    record_case("descriptor-rebind-update", ok, "same layout bound to different UBO buffers, then descriptor update and re-recorded submission");
    if (cmd_pool) p_vkDestroyCommandPool(p->device, cmd_pool, NULL);
    if (pipe) p_vkDestroyPipeline(p->device, pipe, NULL);
    if (pl) p_vkDestroyPipelineLayout(p->device, pl, NULL);
    if (pool) p_vkDestroyDescriptorPool(p->device, pool, NULL);
    if (sl) p_vkDestroyDescriptorSetLayout(p->device, sl, NULL);
    if (in_a) p_vkDestroyBuffer(p->device, in_a, NULL);
    if (in_b) p_vkDestroyBuffer(p->device, in_b, NULL);
    if (out_a) p_vkDestroyBuffer(p->device, out_a, NULL);
    if (out_b) p_vkDestroyBuffer(p->device, out_b, NULL);
    if (out_c) p_vkDestroyBuffer(p->device, out_c, NULL);
    if (mia) p_vkFreeMemory(p->device, mia, NULL);
    if (mib) p_vkFreeMemory(p->device, mib, NULL);
    if (moa) p_vkFreeMemory(p->device, moa, NULL);
    if (mob) p_vkFreeMemory(p->device, mob, NULL);
    if (moc) p_vkFreeMemory(p->device, moc, NULL);
    return ok;
}

static void usage(const wchar_t *exe) {
    LOG("Usage: %ls --loader C:\\absolute\\vulkan-1.dll --icd C:\\absolute\\ashesD3D11Renderer_icd.json [--allow-non-intel-test] [--informational]\n", exe);
}

static int load_device_functions(VkDevice device) {
    LOAD_DEVICE(device, vkDestroyDevice); LOAD_DEVICE(device, vkGetDeviceQueue);
    LOAD_DEVICE(device, vkCreateBuffer); LOAD_DEVICE(device, vkDestroyBuffer); LOAD_DEVICE(device, vkGetBufferMemoryRequirements);
    LOAD_DEVICE(device, vkCreateImage); LOAD_DEVICE(device, vkDestroyImage); LOAD_DEVICE(device, vkGetImageMemoryRequirements);
    LOAD_DEVICE(device, vkAllocateMemory); LOAD_DEVICE(device, vkFreeMemory); LOAD_DEVICE(device, vkBindBufferMemory); LOAD_DEVICE(device, vkBindImageMemory);
    LOAD_DEVICE(device, vkMapMemory); LOAD_DEVICE(device, vkUnmapMemory); LOAD_DEVICE(device, vkFlushMappedMemoryRanges); LOAD_DEVICE(device, vkInvalidateMappedMemoryRanges);
    LOAD_DEVICE(device, vkCreateImageView); LOAD_DEVICE(device, vkDestroyImageView); LOAD_DEVICE(device, vkCreateSampler); LOAD_DEVICE(device, vkDestroySampler);
    LOAD_DEVICE(device, vkCreateDescriptorSetLayout); LOAD_DEVICE(device, vkDestroyDescriptorSetLayout); LOAD_DEVICE(device, vkCreateDescriptorPool); LOAD_DEVICE(device, vkDestroyDescriptorPool); LOAD_DEVICE(device, vkAllocateDescriptorSets); LOAD_DEVICE(device, vkUpdateDescriptorSets);
    LOAD_DEVICE(device, vkCreatePipelineLayout); LOAD_DEVICE(device, vkDestroyPipelineLayout); LOAD_DEVICE(device, vkCreateShaderModule); LOAD_DEVICE(device, vkDestroyShaderModule); LOAD_DEVICE(device, vkCreateComputePipelines); LOAD_DEVICE(device, vkDestroyPipeline);
    LOAD_DEVICE(device, vkCreateCommandPool); LOAD_DEVICE(device, vkDestroyCommandPool); LOAD_DEVICE(device, vkAllocateCommandBuffers); LOAD_DEVICE(device, vkBeginCommandBuffer); LOAD_DEVICE(device, vkEndCommandBuffer); LOAD_DEVICE(device, vkResetCommandBuffer);
    LOAD_DEVICE(device, vkCmdPipelineBarrier); LOAD_DEVICE(device, vkCmdBindPipeline); LOAD_DEVICE(device, vkCmdBindDescriptorSets); LOAD_DEVICE(device, vkCmdDispatch); LOAD_DEVICE(device, vkCmdUpdateBuffer);
    LOAD_DEVICE(device, vkCreateFence); LOAD_DEVICE(device, vkDestroyFence); LOAD_DEVICE(device, vkQueueSubmit); LOAD_DEVICE(device, vkWaitForFences); LOAD_DEVICE(device, vkDeviceWaitIdle);
    NEED(vkDestroyDevice); NEED(vkGetDeviceQueue); NEED(vkCreateBuffer); NEED(vkDestroyBuffer); NEED(vkGetBufferMemoryRequirements);
    NEED(vkCreateImage); NEED(vkDestroyImage); NEED(vkGetImageMemoryRequirements); NEED(vkAllocateMemory); NEED(vkFreeMemory); NEED(vkBindBufferMemory); NEED(vkBindImageMemory);
    NEED(vkMapMemory); NEED(vkUnmapMemory); NEED(vkFlushMappedMemoryRanges); NEED(vkInvalidateMappedMemoryRanges); NEED(vkCreateImageView); NEED(vkDestroyImageView); NEED(vkCreateSampler); NEED(vkDestroySampler);
    NEED(vkCreateDescriptorSetLayout); NEED(vkDestroyDescriptorSetLayout); NEED(vkCreateDescriptorPool); NEED(vkDestroyDescriptorPool); NEED(vkAllocateDescriptorSets); NEED(vkUpdateDescriptorSets);
    NEED(vkCreatePipelineLayout); NEED(vkDestroyPipelineLayout); NEED(vkCreateShaderModule); NEED(vkDestroyShaderModule); NEED(vkCreateComputePipelines); NEED(vkDestroyPipeline);
    NEED(vkCreateCommandPool); NEED(vkDestroyCommandPool); NEED(vkAllocateCommandBuffers); NEED(vkBeginCommandBuffer); NEED(vkEndCommandBuffer); NEED(vkResetCommandBuffer);
    NEED(vkCmdPipelineBarrier); NEED(vkCmdBindPipeline); NEED(vkCmdBindDescriptorSets); NEED(vkCmdDispatch); NEED(vkCmdUpdateBuffer); NEED(vkCreateFence); NEED(vkDestroyFence); NEED(vkQueueSubmit); NEED(vkWaitForFences); NEED(vkDeviceWaitIdle);
    return 1;
}

static int has_device_extension(VkPhysicalDevice physical, const char *wanted) {
    uint32_t count = 0, i;
    VkExtensionProperties *items = NULL;
    int found = 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS || !count) return 0;
    items = (VkExtensionProperties *)calloc(count, sizeof(*items));
    if (!items) return 0;
    if (p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, items) == VK_SUCCESS)
        for (i = 0; i < count; ++i) if (!strcmp(items[i].extensionName, wanted)) { found = 1; break; }
    free(items);
    return found;
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader_path = NULL, *icd_path = NULL;
    int allow_non_intel = 0, status = 1, i, pass, failures_before;
    HMODULE loader = NULL;
    union { FARPROC raw; PFN_vkGetInstanceProcAddr typed; } gipa_union;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice *items = NULL;
    uint32_t count = 0;
    Probe probe;
    VkPhysicalDeviceProperties selected_props;
    const char *first_failure = NULL;
    memset(&probe, 0, sizeof(probe)); memset(&selected_props, 0, sizeof(selected_props));
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--loader") && i + 1 < argc) loader_path = argv[++i];
        else if (!wcscmp(argv[i], L"--icd") && i + 1 < argc) icd_path = argv[++i];
        else if (!wcscmp(argv[i], L"--allow-non-intel-test")) allow_non_intel = 1;
        else if (!wcscmp(argv[i], L"--informational")) g_informational = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { usage(argv[0]); return 0; }
        else { ERR("FAIL: unrecognized or incomplete argument: %ls\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!loader_path || !icd_path || !is_absolute_windows_path(loader_path) || !is_absolute_windows_path(icd_path)) { ERR("FAIL: absolute --loader and --icd are mandatory.\n"); return 2; }
    if (GetFileAttributesW(loader_path) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(icd_path) == INVALID_FILE_ATTRIBUTES) { ERR("FAIL: supplied loader or ICD path is inaccessible (GetLastError=%lu).\n", GetLastError()); return 2; }
    if (GetModuleHandleW(L"vulkan-1.dll")) { ERR("FAIL: vulkan-1.dll was preloaded; refusing ambiguous loader selection.\n"); return 3; }
    if (!SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_path) || !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_path)) { ERR("FAIL: could not isolate the requested ICD.\n"); return 3; }
    LOG("HazardStateProbe: Ashes D3D11 v12 integration diagnostic\nLOADER: %ls\nICD: %ls\n", loader_path, icd_path);
    LOG("CONTRACT: A stale u7->t5 same-mip hazard, B disjoint mip0/mip1, C dynamic b0 offsets, D rebind/update b2 state.\n");
    if (allow_non_intel) LOG("WARNING: --allow-non-intel-test is synthetic integration only, not Intel HD 5500 evidence.\n");
    if (g_informational) LOG("INFO: --informational downgrades all test-case failures; used only for hazard-tracking-off control.\n");
    loader = LoadLibraryW(loader_path);
    if (!loader) { status = 4; first_failure = "LoadLibraryW(loader)"; goto cleanup; }
    gipa_union.raw = GetProcAddress(loader, "vkGetInstanceProcAddr"); gipa = gipa_union.typed;
    if (!gipa) { status = 4; first_failure = "vkGetInstanceProcAddr"; goto cleanup; }
    LOAD_GLOBAL(vkEnumerateInstanceVersion); LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties); LOAD_GLOBAL(vkCreateInstance);
    if (!p_vkCreateInstance) { status = 20; first_failure = "vkCreateInstance entrypoint"; goto cleanup; }
    if (p_vkEnumerateInstanceVersion) { uint32_t version = VK_API_VERSION_1_0; VkResult r = p_vkEnumerateInstanceVersion(&version); LOG("RESULT: vkEnumerateInstanceVersion => %s (%d), max=%u.%u.%u\n", result_name(r), (int)r, VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    if (r != VK_SUCCESS) { status = 21; first_failure = "vkEnumerateInstanceVersion"; goto cleanup; } }
    {
        VkApplicationInfo app; VkInstanceCreateInfo ci; VkResult r;
        memset(&app, 0, sizeof(app)); app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.pApplicationName = "HazardStateProbe"; app.applicationVersion = 1; app.pEngineName = "standalone"; app.engineVersion = 1; app.apiVersion = VK_API_VERSION_1_0;
        memset(&ci, 0, sizeof(ci)); ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo = &app;
        r = p_vkCreateInstance(&ci, NULL, &instance);
        LOG("RESULT: vkCreateInstance(api=1.0) => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateInstance"; goto cleanup; }
    }
    LOAD_INSTANCE(instance, vkDestroyInstance); LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices); LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties); LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties); LOAD_INSTANCE(instance, vkEnumerateDeviceExtensionProperties); LOAD_INSTANCE(instance, vkCreateDevice); LOAD_INSTANCE(instance, vkGetDeviceProcAddr);
    if (!p_vkDestroyInstance || !p_vkEnumeratePhysicalDevices || !p_vkGetPhysicalDeviceProperties || !p_vkGetPhysicalDeviceMemoryProperties || !p_vkGetPhysicalDeviceQueueFamilyProperties || !p_vkEnumerateDeviceExtensionProperties || !p_vkCreateDevice || !p_vkGetDeviceProcAddr) { status = 20; first_failure = "instance entrypoint"; goto cleanup; }
    { VkResult r = p_vkEnumeratePhysicalDevices(instance, &count, NULL); LOG("RESULT: vkEnumeratePhysicalDevices(count) => %s (%d), count=%u\n", result_name(r), (int)r, count);
    if (r != VK_SUCCESS || !count) { status = 5; first_failure = "physical-device enumeration"; goto cleanup; } }
    items = (VkPhysicalDevice *)calloc(count, sizeof(*items));
    if (!items) { status = 5; first_failure = "physical-device list allocation"; goto cleanup; }
    if (p_vkEnumeratePhysicalDevices(instance, &count, items) != VK_SUCCESS) { status = 5; first_failure = "physical-device list enumeration"; goto cleanup; }
    for (i = 0; i < (int)count; ++i) { VkPhysicalDeviceProperties props; p_vkGetPhysicalDeviceProperties(items[i], &props); LOG("ADAPTER[%d]: name=%s vendor=0x%04X device=0x%04X type=%s%s\n", i, props.deviceName, props.vendorID, props.deviceID, device_type_name(props.deviceType), looks_software(&props) ? " [software-like]" : ""); }
    for (pass = 0; pass < (allow_non_intel ? 2 : 1) && !probe.physical; ++pass) {
        for (i = 0; i < (int)count && !probe.physical; ++i) {
            VkPhysicalDeviceProperties props; uint32_t family;
            p_vkGetPhysicalDeviceProperties(items[i], &props);
            if (looks_software(&props)) { LOG("ADAPTER_REJECTED: CPU/software fallback is ineligible.\n"); continue; }
            if ((pass == 0 && props.vendorID != INTEL_VENDOR_ID) || (pass == 1 && props.vendorID == INTEL_VENDOR_ID)) continue;
            if (!find_compute_queue(items[i], &family)) continue;
            probe.physical = items[i]; probe.family = family; selected_props = props;
        }
    }
    if (!probe.physical) { status = 6; first_failure = "eligible non-software adapter (Intel by default)"; goto cleanup; }
    LOG("SELECTED: %s vendor=0x%04X queueFamily=%u policy=%s\n", selected_props.deviceName, selected_props.vendorID, probe.family, allow_non_intel ? "non-Intel-sandbox-override" : "Intel-non-software-required");
    p_vkGetPhysicalDeviceMemoryProperties(probe.physical, &probe.memory);
    {
        float priority = 1.0f; VkDeviceQueueCreateInfo qi; VkDeviceCreateInfo ci; const char *subset = "VK_KHR_portability_subset"; VkResult r;
        memset(&qi, 0, sizeof(qi)); qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueFamilyIndex = probe.family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
        memset(&ci, 0, sizeof(ci)); ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
        if (has_device_extension(probe.physical, subset)) { ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = &subset; }
        r = p_vkCreateDevice(probe.physical, &ci, NULL, &probe.device);
        LOG("RESULT: vkCreateDevice => %s (%d)\n", result_name(r), (int)r);
        if (r != VK_SUCCESS) { status = 21; first_failure = "vkCreateDevice"; goto cleanup; }
    }
    if (!load_device_functions(probe.device)) { status = 20; first_failure = "device entrypoint"; goto cleanup; }
    p_vkGetDeviceQueue(probe.device, probe.family, 0, &probe.queue);
    if (!probe.queue) { status = 7; first_failure = "vkGetDeviceQueue"; goto cleanup; }
    failures_before = g_failures;
    (void)case_stale_uav_to_srv(&probe); (void)case_disjoint_mips(&probe); (void)case_dynamic_offsets(&probe); (void)case_rebind_and_update(&probe);
    if (!vk_ok("vkDeviceWaitIdle", p_vkDeviceWaitIdle(probe.device))) { status = 21; first_failure = "vkDeviceWaitIdle"; goto cleanup; }
    if (!g_informational && g_failures != failures_before) { status = 11; first_failure = "one or more strict test cases"; }
    else status = 0;
cleanup:
    if (probe.device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(probe.device);
    if (probe.device && p_vkDestroyDevice) p_vkDestroyDevice(probe.device, NULL);
    free(items);
    if (instance && p_vkDestroyInstance) p_vkDestroyInstance(instance, NULL);
    if (loader) FreeLibrary(loader);
    if (g_informational) LOG("FINAL: PASS informational; control run recorded without affecting strict evidence.\n");
    else if (status) ERR("FINAL: FAIL status=%d; FIRST_FAILURE: %s\n", status, first_failure ? first_failure : "unspecified");
    else LOG("FINAL: PASS status=0; all strict hazard/state-cache checks matched exact expected values.\n");
    return status;
}
