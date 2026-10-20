/* Direct-ICD smoke test for the HD5500 bridge (test harness, not shipped).
 * Loads ashesD3D11Renderer.dll directly (no Vulkan loader), renders 12 frames:
 * render pass clearing the swapchain image red + one green triangle, then presents.
 * Usage: smoke.exe <path-to-ashesD3D11Renderer.dll> [frames]
 */
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vs.h"
#include "fs.h"
#include "cvs.h"
#include "cfs.h"
#include "pcvs.h"
#include "ssbovs.h"
#include "ssbofs.h"
#include "pubvs.h"
#include "pubfs.h"
#include "vbovs.h"
#include "vbofs.h"
#include "varyvs.h"
#include "varyfs.h"
#include "varybvs.h"
#include "varybfs.h"
#include "varysvs.h"
#include "ssbowcs.h"
#include "watervs.h"
#include "wdepthfs.h"
#include "waterfs.h"
#include "instvs.h"
#include "inst2vs.h"
#include "instfs.h"
#include "inst3vs.h"
#include "inst3fs.h"
#include <math.h>

static PFN_vkGetInstanceProcAddr gipa;
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL: %s -> %d\n", #call, r_); return 1; } } while (0)
#define GI(name) PFN_##name name = (PFN_##name)gipa(instance, #name); if (!name) { printf("FAIL: missing %s\n", #name); return 1; }
#define GD(name) PFN_##name name = (PFN_##name)gdpa(device, #name); if (!name) { printf("FAIL: missing %s\n", #name); return 1; }

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

/* v13.41 helpers for the water mode (host buffers, device images, uploads). */
typedef struct Helper {
	VkDevice device; VkPhysicalDeviceMemoryProperties mp;
	PFN_vkCreateBuffer createBuffer; PFN_vkGetBufferMemoryRequirements bufReq; PFN_vkAllocateMemory alloc; PFN_vkBindBufferMemory bindBuf;
	PFN_vkMapMemory map; PFN_vkUnmapMemory unmap; PFN_vkCreateImage createImage; PFN_vkGetImageMemoryRequirements imgReq;
	PFN_vkBindImageMemory bindImg; PFN_vkCreateImageView createView; PFN_vkCmdPipelineBarrier barrier; PFN_vkCmdCopyBufferToImage copyB2I;
} Helper;
static uint32_t memType(Helper * h, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < h->mp.memoryTypeCount; ++i) if ((bits & (1u << i)) && (h->mp.memoryTypes[i].propertyFlags & want) == want) return i;
	for (uint32_t i = 0; i < h->mp.memoryTypeCount; ++i) if (bits & (1u << i)) return i;
	return 0;
}
static VkResult hostBuffer(Helper * h, VkDeviceSize size, VkBufferUsageFlags usage, const void * init, VkBuffer * out, void ** mapped)
{
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
	VkResult r = h->createBuffer(h->device, &bci, NULL, out); if (r) return r;
	VkMemoryRequirements req; h->bufReq(h->device, *out, &req);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, memType(h, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	VkDeviceMemory mem; r = h->alloc(h->device, &mai, NULL, &mem); if (r) return r;
	r = h->bindBuf(h->device, *out, mem, 0); if (r) return r;
	void * p; r = h->map(h->device, mem, 0, size, 0, &p); if (r) return r;
	if (init) memcpy(p, init, (size_t)size); else memset(p, 0, (size_t)size);
	if (mapped) *mapped = p; else h->unmap(h->device, mem);
	return VK_SUCCESS;
}
static VkResult deviceImage(Helper * h, VkFormat fmt, uint32_t w, uint32_t ht, VkImageUsageFlags usage, VkImage * img, VkImageView * view)
{
	VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, { w, ht, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
	VkResult r = h->createImage(h->device, &ici, NULL, img); if (r) return r;
	VkMemoryRequirements req; h->imgReq(h->device, *img, &req);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, memType(h, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory mem; r = h->alloc(h->device, &mai, NULL, &mem); if (r) return r;
	r = h->bindImg(h->device, *img, mem, 0); if (r) return r;
	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, *img, VK_IMAGE_VIEW_TYPE_2D, fmt, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	return h->createView(h->device, &vci, NULL, view);
}
/* RGBA8 texture filled by gen(x, y, rgba) and uploaded through a staging buffer in cmd. */
typedef void (*TexGen)(uint32_t x, uint32_t y, uint32_t w, uint32_t ht, unsigned char * rgba);
static VkResult uploadTexture(Helper * h, VkCommandBuffer cmd, uint32_t w, uint32_t ht, TexGen gen, VkImage * img, VkImageView * view)
{
	unsigned char * pix = (unsigned char *)malloc(w * ht * 4);
	for (uint32_t y = 0; y < ht; ++y) for (uint32_t x = 0; x < w; ++x) gen(x, y, w, ht, pix + (y * w + x) * 4);
	VkBuffer staging; VkResult r = hostBuffer(h, w * ht * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pix, &staging, NULL); free(pix); if (r) return r;
	r = deviceImage(h, VK_FORMAT_R8G8B8A8_UNORM, w, ht, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, img, view); if (r) return r;
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	h->barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	VkBufferImageCopy c = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { w, ht, 1 } };
	h->copyB2I(cmd, staging, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	h->barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	return VK_SUCCESS;
}
static unsigned char toB(float v) { if (v < 0) v = 0; if (v > 1) v = 1; return (unsigned char)(v * 255.0f + 0.5f); }
static void genRipple(uint32_t x, uint32_t y, uint32_t w, uint32_t ht, unsigned char * p)
{	/* smooth, tileable-ish height pattern in [0.15, 0.85] */
	float fx = (float)x / w * 6.2831853f, fy = (float)y / ht * 6.2831853f;
	float v = 0.5f + 0.2f * sinf(fx * 2.0f + fy) * cosf(fy * 3.0f - fx) + 0.15f * sinf(fx * 5.0f) * sinf(fy * 2.0f);
	p[0] = p[1] = p[2] = toB(v); p[3] = 255;
}
static void genRamp(uint32_t x, uint32_t y, uint32_t w, uint32_t ht, unsigned char * p)
{	/* u = diffuse, v = 1 - specular */
	float d = (x + 0.5f) / w, s = 1.0f - (y + 0.5f) / ht;
	p[0] = toB(0.05f + 0.45f * d + 0.5f * s); p[1] = toB(0.25f + 0.45f * d + 0.5f * s); p[2] = toB(0.45f + 0.3f * d + 0.5f * s); p[3] = 255;
}
static void genHash(uint32_t x, uint32_t y, uint32_t w, uint32_t ht, unsigned char * p)
{	/* v13.42: smooth low-frequency "noise" (was per-texel hash noise, whose anisotropic filtering
	   differs between drivers by up to 26 grey levels at the edge fade) */
	float fx = (float)x / w * 6.2831853f, fy = (float)y / ht * 6.2831853f;
	p[0] = toB(0.5f + 0.3f * sinf(fx * 3.0f + 1.0f) * cosf(fy * 2.0f));
	p[1] = toB(0.5f + 0.3f * sinf(fx * 5.0f - fy * 3.0f));
	p[2] = toB(0.5f + 0.3f * cosf(fx * 2.0f + fy * 7.0f));
	p[3] = 255;
}
static void genGrey(uint32_t x, uint32_t y, uint32_t w, uint32_t ht, unsigned char * p) { p[0] = p[1] = p[2] = 128; p[3] = 255; (void)x; (void)y; (void)w; (void)ht; }
static void genSky(uint32_t x, uint32_t y, uint32_t w, uint32_t ht, unsigned char * p) { float v = 0.4f + 0.2f * ((float)x / w) + 0.1f * ((float)y / ht); p[0] = p[1] = p[2] = toB(v); p[3] = 255; }

int main(int argc, char ** argv)
{
	const char * path = argc > 1 ? argv[1] : "ashesD3D11Renderer.dll";
	int frames = argc > 2 ? atoi(argv[2]) : 12;
	int swizzle = argc > 3 && !strcmp(argv[3], "swizzle");
	int ssbo = argc > 3 && !strcmp(argv[3], "ssbo");
	int dynubo = argc > 3 && !strcmp(argv[3], "dynubo"); /* v13.28: dynamic UBO, element at a non-zero dynamic offset */
	int pushubo = (argc > 3 && !strcmp(argv[3], "pushubo")) || dynubo;
	int composite = (argc > 3 && !strcmp(argv[3], "composite")) || swizzle;
	int pushc = argc > 3 && !strcmp(argv[3], "push");
	int loadpass = argc > 3 && !strcmp(argv[3], "loadpass");
	int clearatt = argc > 3 && !strcmp(argv[3], "clearattach");
	int hdr = argc > 3 && !strcmp(argv[3], "hdrresolve");
	int badblit = argc > 3 && !strcmp(argv[3], "badblit");
	int mipblit = argc > 3 && !strcmp(argv[3], "mipblit");
	int areaclear = argc > 3 && !strcmp(argv[3], "areaclear"); /* v13.26: LOAD_OP_CLEAR with a render area = left half */
	int vbopersist = argc > 3 && !strcmp(argv[3], "vbopersist"); /* v13.33: vertex buffers bound BEFORE a pipeline bind + render pass must still apply */
	int varying = argc > 3 && !strcmp(argv[3], "varying"); /* v13.35: FS reads location 1 only, VS writes 0 and 1 */
	int varyblock = argc > 3 && !strcmp(argv[3], "varyblock"); /* v13.37: VS output block {Color,UV} at loc 0, FS reads loc 1 */
	int varystruct = argc > 3 && !strcmp(argv[3], "varystruct"); /* v13.37: plain struct output (Slang/ImGui style), FS reads loc 1 */
	int water = argc > 3 && !strcmp(argv[3], "water"); /* v13.41: RimWorld's two-pass water (WaterDepth height field -> TerrainWater), real game shaders */
	int readback = getenv("SMOKE_READBACK") == NULL || atoi(getenv("SMOKE_READBACK")) != 0; /* v13.41: CPU readback of the last frame (works on any ICD) */
	int uavlayout = argc > 3 && !strcmp(argv[3], "uavlayout"); /* v13.47: sets bound with a sibling layout, UAV index must follow the pipeline layout */
	int uavdyn = argc > 3 && !strcmp(argv[3], "uavdyn"); /* v13.49: DYNAMIC storage buffer, data at the dynamic offset */
	int uavalias = (argc > 3 && !strcmp(argv[3], "uavalias")); /* v13.53: two SSBO descriptors into ONE buffer (offsets at 0, colours at 256) */
	int uavoffset = (argc > 3 && !strcmp(argv[3], "uavoffset")) || uavdyn; /* v13.47b: storage buffer descriptor with a non-zero offset (ring sub-allocation) */
	int instuav = (argc > 3 && !strcmp(argv[3], "instuav")) || uavlayout || uavoffset || uavalias; /* v13.43: writable SSBO in the VS survives a vkCmdClearAttachments (helper pass) */
	int ssbowrite = argc > 3 && !strcmp(argv[3], "ssbowrite"); /* v13.23: compute writes a host-visible SSBO, CPU checks it; then renders like x */
	int mipsrc = argc > 4 ? atoi(argv[4]) : 1; /* which mip of image 0 goes to the swapchain */
	HMODULE dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!dll) { printf("FAIL: LoadLibrary %lu\n", GetLastError()); return 2; }
	typedef VkResult (VKAPI_PTR * PFN_negotiate)(uint32_t *);
	PFN_negotiate negotiate = (PFN_negotiate)GetProcAddress(dll, "vk_icdNegotiateLoaderICDInterfaceVersion");
	if (negotiate) { uint32_t v = 5; negotiate(&v); printf("ICD interface %u\n", v); }
	gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(dll, "vk_icdGetInstanceProcAddr");
	if (!gipa) { printf("FAIL: no vk_icdGetInstanceProcAddr\n"); return 3; }
	VkInstance instance = VK_NULL_HANDLE;
	GI(vkCreateInstance);
	const char * iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "smoke", 1, "smoke", 1, getenv("SMOKE_API11") ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iext };
	CHECK(vkCreateInstance(&ici, NULL, &instance));
	GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceProperties); GI(vkCreateDevice); GI(vkGetDeviceProcAddr);
	GI(vkCreateWin32SurfaceKHR); GI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); GI(vkGetPhysicalDeviceSurfaceFormatsKHR);
	GI(vkGetPhysicalDeviceSurfaceSupportKHR); GI(vkGetPhysicalDeviceMemoryProperties);
	uint32_t count = 0;
	CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
	if (!count) { printf("FAIL: no devices\n"); return 4; }
	VkPhysicalDevice pds[8]; if (count > 8) count = 8;
	CHECK(vkEnumeratePhysicalDevices(instance, &count, pds));
	/* v13.19: SMOKE_DEVICE=<index> picks the physical device (laptop: 0 = HD 5500, 1 = Microsoft Basic Render Driver / WARP) */
	uint32_t pick = 0; { const char * e = getenv("SMOKE_DEVICE"); if (e) pick = (uint32_t)atoi(e); if (pick >= count) pick = 0; }
	VkPhysicalDevice pd = pds[pick];
	VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s (%u devices)\n", props.deviceName, count);

	WNDCLASSA wc = { 0 }; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "smoke";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowExA(0, "smoke", "smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 656, 519, NULL, NULL, wc.hInstance, NULL);
	VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, wc.hInstance, hwnd };
	VkSurfaceKHR surface; CHECK(vkCreateWin32SurfaceKHR(instance, &sci, NULL, &surface));
	VkBool32 supported = VK_FALSE; CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surface, &supported));

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
	const char * dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dext, NULL };
	VkDevice device; CHECK(vkCreateDevice(pd, &dci, NULL, &device));
	PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
	GD(vkGetDeviceQueue); GD(vkCreateSwapchainKHR); GD(vkGetSwapchainImagesKHR); GD(vkAcquireNextImageKHR); GD(vkQueuePresentKHR);
	GD(vkCreateImageView); GD(vkCreateRenderPass); GD(vkCreateFramebuffer); GD(vkCreateShaderModule); GD(vkCreatePipelineLayout);
	GD(vkCreateGraphicsPipelines); GD(vkCreateCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer);
	GD(vkCmdBeginRenderPass); GD(vkCmdEndRenderPass); GD(vkCmdBindPipeline); GD(vkCmdDraw); GD(vkQueueSubmit); GD(vkDeviceWaitIdle);
	GD(vkResetCommandBuffer); GD(vkCreateSemaphore); GD(vkDestroySwapchainKHR);
	GD(vkCmdPushConstants); GD(vkCmdClearAttachments); GD(vkCmdBlitImage); GD(vkCmdClearColorImage); GD(vkCreateBuffer); GD(vkCmdBindVertexBuffers); GD(vkGetBufferMemoryRequirements); GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkUnmapMemory); GD(vkCmdPipelineBarrier); GD(vkCmdSetScissor); GD(vkCreateImage); GD(vkGetImageMemoryRequirements); GD(vkAllocateMemory); GD(vkBindImageMemory); GD(vkCreateSampler);
	GD(vkCreateDescriptorSetLayout); GD(vkCreateDescriptorPool); GD(vkAllocateDescriptorSets); GD(vkUpdateDescriptorSets); GD(vkCmdBindDescriptorSets);
	GD(vkCmdCopyBufferToImage); GD(vkCmdCopyImageToBuffer); GD(vkCmdBindIndexBuffer); GD(vkCmdDrawIndexed); GD(vkCmdDrawIndexedIndirect); GD(vkCmdCopyBuffer);
	VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);

	/* v13.49 api11: the Vulkan 1.1 entry points Waylith uses when the device reports 1.1+
	   (VMA *2 memory functions, Properties2 with ID/driver chains, GetDeviceQueue2,
	   DescriptorSetLayoutSupport). Prints api11-check lines; FAIL on a wrong answer. */
	if (getenv("SMOKE_API11"))
	{
		PFN_vkGetPhysicalDeviceProperties2 gpp2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
		PFN_vkGetBufferMemoryRequirements2 gbmr2 = (PFN_vkGetBufferMemoryRequirements2)gdpa(device, "vkGetBufferMemoryRequirements2");
		PFN_vkGetDeviceQueue2 gdq2 = (PFN_vkGetDeviceQueue2)gdpa(device, "vkGetDeviceQueue2");
		PFN_vkGetDescriptorSetLayoutSupport gdsls = (PFN_vkGetDescriptorSetLayoutSupport)gdpa(device, "vkGetDescriptorSetLayoutSupport");
		int ok = gpp2 && gbmr2 && gdq2 && gdsls;
		printf("api11-check entry points %s, reported apiVersion %u.%u\n", ok ? "present" : "MISSING", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));
		if (ok)
		{
			VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
			VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &drv };
			VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
			gpp2(pd, &p2);
			int chainKept = p2.pNext == &idp && idp.pNext == &drv;
			printf("api11-check properties2 chain %s, uuid[0..3]=%02x%02x%02x%02x driver='%s'\n", chainKept ? "kept" : "BROKEN", idp.deviceUUID[0], idp.deviceUUID[1], idp.deviceUUID[2], idp.deviceUUID[3], drv.driverName);
			VkBuffer b; VkBufferCreateInfo bci2 = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
			CHECK(vkCreateBuffer(device, &bci2, NULL, &b));
			VkMemoryRequirements r1; vkGetBufferMemoryRequirements(device, b, &r1);
			VkMemoryDedicatedRequirements ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, NULL, 7, 7 };
			VkMemoryRequirements2 r2 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &ded };
			VkBufferMemoryRequirementsInfo2 bi = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, NULL, b };
			gbmr2(device, &bi, &r2);
			int memOk = r2.memoryRequirements.size == r1.size && r2.memoryRequirements.memoryTypeBits == r1.memoryTypeBits && ded.prefersDedicatedAllocation == 0 && ded.requiresDedicatedAllocation == 0;
			printf("api11-check memory requirements2 %s (size %llu)\n", memOk ? "match" : "WRONG", (unsigned long long)r2.memoryRequirements.size);
			VkQueue q2 = VK_NULL_HANDLE; VkDeviceQueueInfo2 qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, NULL, 0, 0, 0 };
			gdq2(device, &qi, &q2);
			VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
			VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &lb };
			VkDescriptorSetLayoutSupport sup = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT };
			gdsls(device, &lci, &sup);
			printf("api11-check queue2 %s, layout support %u\n", q2 == queue ? "same queue" : "DIFFERENT", sup.supported);
			ok = chainKept && memOk && q2 == queue && sup.supported;
		}
		printf(ok ? "api11-check PASS\n" : "api11-check FAIL\n");
	}

	/* v13.23 ssbowrite: a WRITABLE storage buffer must still be copied back to the CPU after
	   the bridge stopped downloading read-only storage buffers. Compute writes v[i] = 3i+7. */
	if (ssbowrite)
	{
		GD(vkCreateComputePipelines); GD(vkCmdDispatch);
		VkDescriptorSetLayoutBinding wb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
		VkDescriptorSetLayoutCreateInfo wdslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &wb };
		VkDescriptorSetLayout wdsl; CHECK(vkCreateDescriptorSetLayout(device, &wdslci, NULL, &wdsl));
		VkPipelineLayoutCreateInfo wplci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &wdsl, 0, NULL };
		VkPipelineLayout wpl; CHECK(vkCreatePipelineLayout(device, &wplci, NULL, &wpl));
		VkShaderModuleCreateInfo wsm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(ssbowcs_spv), ssbowcs_spv };
		VkShaderModule wmod; CHECK(vkCreateShaderModule(device, &wsm, NULL, &wmod));
		VkComputePipelineCreateInfo wcpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, wmod, "main", NULL }, wpl, VK_NULL_HANDLE, -1 };
		VkPipeline wpipe; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &wcpci, NULL, &wpipe));
		VkBufferCreateInfo wbci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
		VkBuffer wbuf; CHECK(vkCreateBuffer(device, &wbci, NULL, &wbuf));
		VkMemoryRequirements wreq; vkGetBufferMemoryRequirements(device, wbuf, &wreq);
		VkPhysicalDeviceMemoryProperties wmp; vkGetPhysicalDeviceMemoryProperties(pd, &wmp);
		uint32_t wtype = 0; for (uint32_t i = 0; i < wmp.memoryTypeCount; ++i) if ((wreq.memoryTypeBits & (1u << i)) && (wmp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { wtype = i; break; }
		VkMemoryAllocateInfo wmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, wreq.size, wtype };
		VkDeviceMemory wmem; CHECK(vkAllocateMemory(device, &wmai, NULL, &wmem)); CHECK(vkBindBufferMemory(device, wbuf, wmem, 0));
		uint32_t * wdata; CHECK(vkMapMemory(device, wmem, 0, 256 * 4, 0, (void **)&wdata));
		memset(wdata, 0, 256 * 4);
		VkDescriptorPoolSize wps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
		VkDescriptorPoolCreateInfo wdpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &wps };
		VkDescriptorPool wpool; CHECK(vkCreateDescriptorPool(device, &wdpci, NULL, &wpool));
		VkDescriptorSetAllocateInfo wdsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, wpool, 1, &wdsl };
		VkDescriptorSet wset; CHECK(vkAllocateDescriptorSets(device, &wdsai, &wset));
		VkDescriptorBufferInfo wdbi = { wbuf, 0, 256 * 4 };
		VkWriteDescriptorSet wwr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wset, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &wdbi, NULL };
		vkUpdateDescriptorSets(device, 1, &wwr, 0, NULL);
		VkCommandPoolCreateInfo wcpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
		VkCommandPool wcp; CHECK(vkCreateCommandPool(device, &wcpi, NULL, &wcp));
		VkCommandBufferAllocateInfo wcai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, wcp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
		VkCommandBuffer wcmd; CHECK(vkAllocateCommandBuffers(device, &wcai, &wcmd));
		VkCommandBufferBeginInfo wbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, NULL };
		CHECK(vkBeginCommandBuffer(wcmd, &wbi));
		vkCmdBindPipeline(wcmd, VK_PIPELINE_BIND_POINT_COMPUTE, wpipe);
		vkCmdBindDescriptorSets(wcmd, VK_PIPELINE_BIND_POINT_COMPUTE, wpl, 0, 1, &wset, 0, NULL);
		vkCmdDispatch(wcmd, 4, 1, 1);
		CHECK(vkEndCommandBuffer(wcmd));
		VkSubmitInfo wsi = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &wcmd, 0, NULL };
		CHECK(vkQueueSubmit(queue, 1, &wsi, VK_NULL_HANDLE));
		CHECK(vkDeviceWaitIdle(device));
		int bad = 0; for (uint32_t i = 0; i < 256; ++i) if (wdata[i] != i * 3u + 7u) { if (bad < 4) printf("ssbowrite: v[%u]=%u expected %u\n", i, wdata[i], i * 3u + 7u); ++bad; }
		if (bad) { printf("FAIL: ssbowrite %d of 256 values not visible to the CPU\n", bad); return 1; }
		printf("ssbowrite: all 256 values written by the compute shader are visible to the CPU\n");
	}

	VkSurfaceCapabilitiesKHR caps; CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
	VkSurfaceFormatKHR fmts[64]; count = 64; CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, fmts));
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
	VkExtent2D extent = caps.currentExtent;
	if (extent.width == UINT32_MAX) { extent.width = 640; extent.height = 480; }
	uint32_t want = caps.minImageCount + 1; if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
	printf("caps: images %u..%u extent %ux%u -> requesting %u\n", caps.minImageCount, caps.maxImageCount, extent.width, extent.height, want);
	VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, want, format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		extent, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE };
	VkSwapchainKHR swapchain; CHECK(vkCreateSwapchainKHR(device, &swci, NULL, &swapchain));
	if (getenv("SMOKE_RECREATE")) {   /* v13.60: recreate with oldSwapchain like a window resize (flip-model retire path) */
		VkSwapchainCreateInfoKHR again = swci; again.oldSwapchain = swapchain;
		VkSwapchainKHR replacement; CHECK(vkCreateSwapchainKHR(device, &again, NULL, &replacement));
		vkDestroySwapchainKHR(device, swapchain, NULL);
		swapchain = replacement;
		printf("swapchain recreated with oldSwapchain\n");
	}
	VkImage images[4]; count = 4; CHECK(vkGetSwapchainImagesKHR(device, swapchain, &count, images));
	uint32_t imageCount = count;
	printf("swapchain images: %u\n", imageCount);

	VkAttachmentDescription att = { 0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL };
	VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 0, NULL };
	VkRenderPass rp; CHECK(vkCreateRenderPass(device, &rpci, NULL, &rp));
	VkFramebuffer fbs[4], fbs2[4];
	VkAttachmentDescription att2 = att; att2.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att2.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkRenderPassCreateInfo rpci2 = rpci; rpci2.pAttachments = &att2;
	VkRenderPass rp2; CHECK(vkCreateRenderPass(device, &rpci2, NULL, &rp2));
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, images[i], VK_IMAGE_VIEW_TYPE_2D, format, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		VkImageView view; CHECK(vkCreateImageView(device, &ivci, NULL, &view));
		VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, extent.width, extent.height, 1 };
		CHECK(vkCreateFramebuffer(device, &fci, NULL, &fbs[i]));
		fci.renderPass = rp2; CHECK(vkCreateFramebuffer(device, &fci, NULL, &fbs2[i]));
	}
	VkShaderModuleCreateInfo vsm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(vs_spv), vs_spv };
	if (pushc) { vsm.codeSize = sizeof(pcvs_spv); vsm.pCode = pcvs_spv; }
	if (ssbo) { vsm.codeSize = sizeof(ssbovs_spv); vsm.pCode = ssbovs_spv; }
	if (pushubo) { vsm.codeSize = sizeof(pubvs_spv); vsm.pCode = pubvs_spv; }
	VkShaderModuleCreateInfo fsm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(fs_spv), fs_spv };
	if (ssbo) { fsm.codeSize = sizeof(ssbofs_spv); fsm.pCode = ssbofs_spv; }
	if (pushubo) { fsm.codeSize = sizeof(pubfs_spv); fsm.pCode = pubfs_spv; }
	if (varyblock) { vsm.codeSize = sizeof(varybvs_spv); vsm.pCode = varybvs_spv; fsm.codeSize = sizeof(varybfs_spv); fsm.pCode = varybfs_spv; printf("varyblock mode: VS block {Color,UV} at location 0, FS reads location 1 (UV=(0,1)); expect a BLUE triangle\n"); }
	if (varystruct) { vsm.codeSize = sizeof(varysvs_spv); vsm.pCode = varysvs_spv; fsm.codeSize = sizeof(varybfs_spv); fsm.pCode = varybfs_spv; printf("varystruct mode: VS struct output {Color,UV} at location 0 (no Block), FS reads location 1; expect a BLUE triangle\n"); }
	if (varying) { vsm.codeSize = sizeof(varyvs_spv); vsm.pCode = varyvs_spv; fsm.codeSize = sizeof(varyfs_spv); fsm.pCode = varyfs_spv; printf("varying mode: VS writes green@0 and blue@1, FS reads only location 1; expect a BLUE triangle\n"); }
	VkShaderModule vsmod, fsmod; CHECK(vkCreateShaderModule(device, &vsm, NULL, &vsmod)); CHECK(vkCreateShaderModule(device, &fsm, NULL, &fsmod));
	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	if (pushc) { plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr; }
	/* v13.15 ssbo mode: vertex shader reads per-instance offset+colour from a readonly storage buffer, 4 instances */
	VkDescriptorSet ssboSet = VK_NULL_HANDLE; VkDescriptorSetLayout ssboDsl = VK_NULL_HANDLE;
	if (ssbo)
	{
		VkDescriptorSetLayoutBinding sb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
		VkDescriptorSetLayoutCreateInfo sdslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &sb };
		CHECK(vkCreateDescriptorSetLayout(device, &sdslci, NULL, &ssboDsl));
		plci.setLayoutCount = 1; plci.pSetLayouts = &ssboDsl;
		VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 8 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
		VkBuffer sbuf; CHECK(vkCreateBuffer(device, &bci, NULL, &sbuf));
		VkMemoryRequirements breq; vkGetBufferMemoryRequirements(device, sbuf, &breq);
		VkPhysicalDeviceMemoryProperties bmp; vkGetPhysicalDeviceMemoryProperties(pd, &bmp);
		uint32_t btype = 0; for (uint32_t i = 0; i < bmp.memoryTypeCount; ++i) if ((breq.memoryTypeBits & (1u << i)) && (bmp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { btype = i; break; }
		VkMemoryAllocateInfo bmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, breq.size, btype };
		VkDeviceMemory bmem; CHECK(vkAllocateMemory(device, &bmai, NULL, &bmem)); CHECK(vkBindBufferMemory(device, sbuf, bmem, 0));
		float * data; CHECK(vkMapMemory(device, bmem, 0, 8 * 16, 0, (void **)&data));
		float init[32] = { -0.5f,-0.5f,0,0,  0.5f,-0.5f,0,0,  -0.5f,0.5f,0,0,  0.5f,0.5f,0,0,
			1,1,0,1,  0,1,0,1,  0,0,1,1,  1,1,1,1 };
		memcpy(data, init, sizeof(init)); vkUnmapMemory(device, bmem);
		VkDescriptorPoolSize sps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
		VkDescriptorPoolCreateInfo sdpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &sps };
		VkDescriptorPool spool; CHECK(vkCreateDescriptorPool(device, &sdpci, NULL, &spool));
		VkDescriptorSetAllocateInfo sdsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, spool, 1, &ssboDsl };
		CHECK(vkAllocateDescriptorSets(device, &sdsai, &ssboSet));
		VkDescriptorBufferInfo dbi = { sbuf, 0, 8 * 16 };
		VkWriteDescriptorSet sw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ssboSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL };
		vkUpdateDescriptorSets(device, 1, &sw, 0, NULL);
		printf("ssbo mode: 4 instanced triangles, offsets+colours from a readonly SSBO in the VS\n");
	}
	/* v13.17 pushubo mode: UBO b0 visible to VS|FS (colour, read by FS), UBO binding 1 VS-only (scale), push offset.
	   Expect a white triangle shifted to the right half (offset +0.5,0). */
	float * dynData = NULL; /* v13.28 dynubo: persistently mapped UBO, rewritten every frame */
	VkDescriptorSet pubSet = VK_NULL_HANDLE; VkDescriptorSetLayout pubDsl = VK_NULL_HANDLE; VkPushConstantRange pubPcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
	if (pushubo)
	{
		VkDescriptorSetLayoutBinding ub[2] = { { 0, dynubo ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
			{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL } };
		VkDescriptorSetLayoutCreateInfo udslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, ub };
		CHECK(vkCreateDescriptorSetLayout(device, &udslci, NULL, &pubDsl));
		plci.setLayoutCount = 1; plci.pSetLayouts = &pubDsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pubPcr;
		VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
		VkBuffer ubuf; CHECK(vkCreateBuffer(device, &bci, NULL, &ubuf));
		VkMemoryRequirements breq; vkGetBufferMemoryRequirements(device, ubuf, &breq);
		VkPhysicalDeviceMemoryProperties bmp; vkGetPhysicalDeviceMemoryProperties(pd, &bmp);
		uint32_t btype = 0; for (uint32_t i = 0; i < bmp.memoryTypeCount; ++i) if ((breq.memoryTypeBits & (1u << i)) && (bmp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { btype = i; break; }
		VkMemoryAllocateInfo bmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, breq.size, btype };
		VkDeviceMemory bmem; CHECK(vkAllocateMemory(device, &bmai, NULL, &bmem)); CHECK(vkBindBufferMemory(device, ubuf, bmem, 0));
		float * data; CHECK(vkMapMemory(device, bmem, 0, 1024, 0, (void **)&data));
		memset(data, 0, 1024); data[0] = data[1] = data[2] = data[3] = 1.0f;   /* colour white at offset 0 */
		if (dynubo) { data[0] = data[1] = data[2] = 0.0f; data[128] = 0.0f; data[129] = data[130] = data[131] = 1.0f; } /* dynubo: black at 0, cyan at 512 */
		data[64] = data[65] = 0.5f; data[66] = data[67] = 1.0f;               /* scale 0.5 at offset 256 */
		if (dynubo) dynData = data; if (!dynubo) vkUnmapMemory(device, bmem); /* dynubo keeps the memory mapped, like Waylith */
		VkDescriptorPoolSize ups[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 } };
		VkDescriptorPoolCreateInfo udpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ups };
		VkDescriptorPool upool; CHECK(vkCreateDescriptorPool(device, &udpci, NULL, &upool));
		VkDescriptorSetAllocateInfo udsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, upool, 1, &pubDsl };
		CHECK(vkAllocateDescriptorSets(device, &udsai, &pubSet));
		VkDescriptorBufferInfo dbi[2] = { { ubuf, 0, 16 }, { ubuf, 256, 16 } };
		VkWriteDescriptorSet uw[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, pubSet, 0, 0, 1, dynubo ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &dbi[0], NULL },
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, pubSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &dbi[1], NULL } };
		vkUpdateDescriptorSets(device, 2, uw, 0, NULL);
		printf("pushubo mode: UBO b0 (VS|FS, colour), UBO b1 (VS, scale), push offset +0.5 -> white triangle in the right half\n");
	}
	VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vsmod, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsmod, "main", NULL } };
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
	VkViewport vp = { 0, 0, (float)extent.width, (float)extent.height, 0, 1 };
	VkRect2D sc = { { 0, 0 }, extent };
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { VK_FALSE, 0, 0, 0, 0, 0, 0, 0xF };
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba, { 0 } };
	VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms, NULL, &cb, NULL, layout, rp, 0, VK_NULL_HANDLE, -1 };
	VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline));
	/* v13.33 vbopersist: a pipeline with two vertex bindings (0: per-vertex position, stride 8;
	   1: per-INSTANCE colour, stride 48, attribute at offset 16, like Waylith's default-attribute
	   buffer). The buffers are bound once, before a different pipeline and before the render
	   pass; Vulkan keeps that binding, so the draw must still see them. Colour buffer bound at
	   offset 48: bytes 48..64 = green decoy, 64..80 = blue (what offset+attribute offset selects). */
	VkPipeline vbopipe = VK_NULL_HANDLE; VkBuffer vboPos = VK_NULL_HANDLE, vboCol = VK_NULL_HANDLE;
	if (vbopersist)
	{
		VkShaderModuleCreateInfo vvsm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(vbovs_spv), vbovs_spv };
		VkShaderModuleCreateInfo vfsm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(vbofs_spv), vbofs_spv };
		VkShaderModule vvs, vfs; CHECK(vkCreateShaderModule(device, &vvsm, NULL, &vvs)); CHECK(vkCreateShaderModule(device, &vfsm, NULL, &vfs));
		VkPipelineShaderStageCreateInfo vst[2] = { stages[0], stages[1] }; vst[0].module = vvs; vst[1].module = vfs;
		VkVertexInputBindingDescription vbind[2] = { { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 48, VK_VERTEX_INPUT_RATE_INSTANCE } };
		VkVertexInputAttributeDescription vattr[2] = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 } };
		VkPipelineVertexInputStateCreateInfo vvi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 2, vbind, 2, vattr };
		VkGraphicsPipelineCreateInfo vgp = gpci; vgp.pStages = vst; vgp.pVertexInputState = &vvi;
		CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &vgp, NULL, &vbopipe));
		VkPhysicalDeviceMemoryProperties vmp; vkGetPhysicalDeviceMemoryProperties(pd, &vmp);
		for (int which = 0; which < 2; ++which)
		{
			VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
			VkBuffer buf; CHECK(vkCreateBuffer(device, &bci, NULL, &buf));
			VkMemoryRequirements breq; vkGetBufferMemoryRequirements(device, buf, &breq);
			uint32_t btype = 0; for (uint32_t i = 0; i < vmp.memoryTypeCount; ++i) if ((breq.memoryTypeBits & (1u << i)) && (vmp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { btype = i; break; }
			VkMemoryAllocateInfo bmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, breq.size, btype };
			VkDeviceMemory bmem; CHECK(vkAllocateMemory(device, &bmai, NULL, &bmem)); CHECK(vkBindBufferMemory(device, buf, bmem, 0));
			float * data; CHECK(vkMapMemory(device, bmem, 0, 256, 0, (void **)&data));
			memset(data, 0, 256);
			if (which == 0) { float tri[6] = { -0.6f, 0.6f, 0.6f, 0.6f, 0.0f, -0.6f }; memcpy(data, tri, sizeof(tri)); vboPos = buf; }
			else { data[12] = 0; data[13] = 1; data[14] = 0; data[15] = 1; /* decoy green at 48 */ data[16] = 0; data[17] = 0; data[18] = 1; data[19] = 1; /* blue at 64 */ vboCol = buf; }
			vkUnmapMemory(device, bmem);
		}
		printf("vbopersist mode: vertex buffers bound before pipeline bind + render pass; expect a BLUE triangle\n");
	}
	VkRenderPass orp = VK_NULL_HANDLE; VkFramebuffer ofb = VK_NULL_HANDLE; VkPipeline opipe = VK_NULL_HANDLE, cpipe = VK_NULL_HANDLE;
	VkPipelineLayout clayout = VK_NULL_HANDLE; VkDescriptorSet dset = VK_NULL_HANDLE;
	VkImage hdrimg = VK_NULL_HANDLE;
	if (composite || hdr)
	{
		VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM, { extent.width, extent.height, 1 }, 1, 1,
			VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
		VkImage oimg; CHECK(vkCreateImage(device, &imci, NULL, &oimg)); hdrimg = oimg;
		VkMemoryRequirements req; vkGetImageMemoryRequirements(device, oimg, &req);
		VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		uint32_t type = 0; for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
		VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type };
		VkDeviceMemory omem; CHECK(vkAllocateMemory(device, &mai, NULL, &omem)); CHECK(vkBindImageMemory(device, oimg, omem, 0));
		VkImageViewCreateInfo oivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, oimg, VK_IMAGE_VIEW_TYPE_2D, imci.format, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		VkImageView oview; CHECK(vkCreateImageView(device, &oivci, NULL, &oview));
		/* v13.14 swizzle mode: the sampled view maps (R,R,R,1): red triangle -> white, grey stays grey */
		VkImageView sview = oview;
		if (swizzle)
		{
			VkImageViewCreateInfo svci = oivci;
			svci.components.r = VK_COMPONENT_SWIZZLE_R; svci.components.g = VK_COMPONENT_SWIZZLE_R; svci.components.b = VK_COMPONENT_SWIZZLE_R; svci.components.a = VK_COMPONENT_SWIZZLE_ONE;
			CHECK(vkCreateImageView(device, &svci, NULL, &sview));
			printf("swizzle mode: sampled view components (R,R,R,ONE)\n");
		}
		VkAttachmentDescription oatt = att; oatt.format = imci.format; oatt.finalLayout = hdr ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkRenderPassCreateInfo orpci = rpci; orpci.pAttachments = &oatt;
		CHECK(vkCreateRenderPass(device, &orpci, NULL, &orp));
		VkFramebufferCreateInfo ofci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, orp, 1, &oview, extent.width, extent.height, 1 };
		CHECK(vkCreateFramebuffer(device, &ofci, NULL, &ofb));
		VkGraphicsPipelineCreateInfo ogp = gpci; ogp.renderPass = orp;
		CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ogp, NULL, &opipe));
		VkSamplerCreateInfo smci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
			VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_FALSE };
		VkSampler sampler; CHECK(vkCreateSampler(device, &smci, NULL, &sampler));
		VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
		VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
		VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl));
		VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
		VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
		VkDescriptorPool dpool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &dpool));
		VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool, 1, &dsl };
		CHECK(vkAllocateDescriptorSets(device, &dsai, &dset));
		VkDescriptorImageInfo dii = { sampler, sview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, dset, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, NULL, NULL };
		vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
		VkPipelineLayoutCreateInfo cplci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL };
		CHECK(vkCreatePipelineLayout(device, &cplci, NULL, &clayout));
		VkShaderModuleCreateInfo cvsm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(cvs_spv), cvs_spv };
		VkShaderModuleCreateInfo cfsm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(cfs_spv), cfs_spv };
		VkShaderModule cvs, cfs; CHECK(vkCreateShaderModule(device, &cvsm, NULL, &cvs)); CHECK(vkCreateShaderModule(device, &cfsm, NULL, &cfs));
		VkPipelineShaderStageCreateInfo cst[2] = { stages[0], stages[1] }; cst[0].module = cvs; cst[1].module = cfs;
		VkGraphicsPipelineCreateInfo cgp = gpci; cgp.pStages = cst; cgp.layout = clayout;
		CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &cgp, NULL, &cpipe));
		printf("composite mode: offscreen R8G8B8A8 + sampled fullscreen pass\n");
	}
	/* v13.11 mipblit: scaled linear blits mip0->mip1 on formats RimWorld's Mipmapper feeds
	   (R8_UNORM=9, R16G16B16A16_UNORM=91, R32_SFLOAT=100, R32G32_SFLOAT=103, R32G32B32A32_SFLOAT=109),
	   then an up-scaled blit of the R8 mip1 into the swapchain (expect ~0x80 grey). */
	VkFormat mipFormats[] = { argc > 5 ? (VkFormat)atoi(argv[5]) : VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
	enum { NMIP = 5 };
	VkImage mipimg[NMIP] = { 0 };
	if (mipblit)
	{
		VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		for (int i = 0; i < NMIP; ++i)
		{
			VkImageCreateInfo mci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, mipFormats[i], { 64, 64, 1 }, 2, 1,
				VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
			CHECK(vkCreateImage(device, &mci, NULL, &mipimg[i]));
			VkMemoryRequirements req; vkGetImageMemoryRequirements(device, mipimg[i], &req);
			uint32_t type = 0; for (uint32_t k = 0; k < mp.memoryTypeCount; ++k) if ((req.memoryTypeBits & (1u << k)) && (mp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = k; break; }
			VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type };
			VkDeviceMemory mem; CHECK(vkAllocateMemory(device, &mai, NULL, &mem)); CHECK(vkBindImageMemory(device, mipimg[i], mem, 0));
		}
		printf("mipblit mode: 5 formats, mip0->mip1 scaled linear, then R8 mip1 -> swapchain\n");
	}

	/* v13.41 water mode: RimWorld's water is drawn in two passes with the game's own shaders.
	   Pass A (WaterDepth): the water cells are drawn into an R16_SFLOAT "height field" the size
	   of the screen, blending SRC_ALPHA/ONE_MINUS_SRC_ALPHA; the fragment shader multiplies four
	   rotated/scrolled samples of a ripple texture. Pass B (TerrainWater): the same cells are
	   drawn to the swapchain; the shader reconstructs a normal from the height-field gradient
	   (4 taps, textureSize, dFdx/dFdy), lights it and adds reflections + noise.
	   The expected picture is a rippled blue/grey surface with visible variation; a flat,
	   uniform colour means the height field or the normal reconstruction is broken. */
	Helper H = { device, { 0 }, vkCreateBuffer, vkGetBufferMemoryRequirements, vkAllocateMemory, vkBindBufferMemory, vkMapMemory, vkUnmapMemory,
		vkCreateImage, vkGetImageMemoryRequirements, vkBindImageMemory, vkCreateImageView, vkCmdPipelineBarrier, vkCmdCopyBufferToImage };
	vkGetPhysicalDeviceMemoryProperties(pd, &H.mp);
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
	VkCommandPool pool; CHECK(vkCreateCommandPool(device, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));
	enum { WCELLS_X = 8, WCELLS_Z = 6, WINDICES = WCELLS_X * WCELLS_Z * 6 };
	VkPipeline wdepthPipe = VK_NULL_HANDLE, waterPipe = VK_NULL_HANDLE; VkPipelineLayout wdepthLayout = VK_NULL_HANDLE, waterLayout = VK_NULL_HANDLE;
	VkDescriptorSet wset0 = VK_NULL_HANDLE, wsetDepth = VK_NULL_HANDLE, wsetWater = VK_NULL_HANDLE, wsetCopy = VK_NULL_HANDLE, wsetOther = VK_NULL_HANDLE;
	int const setcopy = getenv("SMOKE_SETCOPY") != NULL;
	/* v13.75 SMOKE_UBOWIN: the water pass in two halves from two new sets that differ only in the
	   uniform-buffer OFFSET (same buffer, same textures; the second window tints the water red), the way
	   Waylith writes a new set per draw for its per-draw uniforms. Must equal the old bridge path. */
	int const ubowin = getenv("SMOKE_UBOWIN") != NULL; VkDescriptorSet wsetU0 = VK_NULL_HANDLE, wsetU1 = VK_NULL_HANDLE;   /* v13.73: new set handle with identical content (content fast path) */
	VkRenderPass wrp = VK_NULL_HANDLE; VkFramebuffer wfb = VK_NULL_HANDLE; VkBuffer wvbPos = VK_NULL_HANDLE, wvbCol = VK_NULL_HANDLE, wib = VK_NULL_HANDLE, wind = VK_NULL_HANDLE;
	int const ibofs = getenv("SMOKE_IBOFS") != NULL; enum { WPAD = 36 };
	/* v13.74 SMOKE_IDXMULTI: the water pass drawn from two more index buffers (first half from a
	   persistently mapped host buffer, second half from a device-local buffer filled by
	   vkCmdCopyBuffer every frame). Frame 1 writes wrong indices into both, later frames the right
	   ones: a stale copy (index pool) would leave frame 1's triangles in the final picture. */
	int const idxmulti = getenv("SMOKE_IDXMULTI") != NULL;
	VkBuffer ibA = VK_NULL_HANDLE, ibB = VK_NULL_HANDLE, ibBStage = VK_NULL_HANDLE; uint32_t * ibAMap = NULL, * ibBStageMap = NULL; uint32_t idxKeep[WINDICES];
	if (water)
	{
		/* geometry, laid out exactly like the game's terrain meshes (input layout seen in the logs:
		   binding 0 stride 16 = float3 position + Color32 R8G8B8A8_UNORM at offset 12; binding 1 is
		   Waylith's per-instance default-attribute buffer, stride 16, read by nothing here):
		   WCELLS_X x WCELLS_Z unit cells at y = 0, colour white, alpha (= depth weight) 1, outer ring 0.5 */
		unsigned char vtx[WCELLS_X * WCELLS_Z * 4 * 16]; uint32_t idx[WINDICES];
		for (int cz = 0, n = 0; cz < WCELLS_Z; ++cz) for (int cx = 0; cx < WCELLS_X; ++cx, ++n)
		{
			float corners[4][2] = { { (float)cx, (float)cz }, { (float)cx, (float)cz + 1 }, { (float)cx + 1, (float)cz + 1 }, { (float)cx + 1, (float)cz } };
			unsigned char a = (cx == 0 || cz == 0 || cx == WCELLS_X - 1 || cz == WCELLS_Z - 1) ? 128 : 255;
			for (int v = 0; v < 4; ++v) { unsigned char * p = vtx + (n * 4 + v) * 16; float xyz[3] = { corners[v][0], 0.0f, corners[v][1] }; memcpy(p, xyz, 12); p[12] = p[13] = p[14] = 255; p[15] = a; }
			uint32_t b = n * 4; uint32_t * t = idx + n * 6; t[0] = b; t[1] = b + 1; t[2] = b + 2; t[3] = b; t[4] = b + 2; t[5] = b + 3;
		}
		CHECK(hostBuffer(&H, sizeof(vtx), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vtx, &wvbPos, NULL));
		CHECK(hostBuffer(&H, 256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, NULL, &wvbCol, NULL)); /* default-attribute buffer, zeros */
		if (ibofs)
		{
			/* v13.66: SMOKE_IBOFS=1 - the same indices behind WPAD wrong ones (shifted by one, so a
			   wrong offset draws visibly different triangles); drawn in two halves at non-zero
			   index-buffer offsets, the second half by vkCmdDrawIndexedIndirect. Must match the
			   reference picture exactly (tests the bridge's index-offset folding). */
			uint32_t padded[WPAD + WINDICES];
			for (int i = 0; i < WPAD + WINDICES; ++i) padded[i] = i < WPAD ? (uint32_t)((i + 1) % (WCELLS_X * WCELLS_Z * 4)) : idx[i - WPAD];
			CHECK(hostBuffer(&H, sizeof(padded), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, padded, &wib, NULL));
			uint32_t ind[5] = { WINDICES / 2, 1, getenv("SMOKE_IBOFS")[0] == '3' ? WPAD + WINDICES / 2 : 0, 0, 0 };
			CHECK(hostBuffer(&H, sizeof(ind), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, ind, &wind, NULL));
		}
		else
		CHECK(hostBuffer(&H, sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, idx, &wib, NULL));
		if (idxmulti) {
			memcpy(idxKeep, idx, sizeof(idx));
			CHECK(hostBuffer(&H, sizeof(idx) / 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, NULL, &ibA, (void **)&ibAMap));
			CHECK(hostBuffer(&H, sizeof(idx) / 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, NULL, &ibBStage, (void **)&ibBStageMap));
			VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sizeof(idx) / 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
			CHECK(H.createBuffer(device, &bci, NULL, &ibB));
			VkMemoryRequirements req; H.bufReq(device, ibB, &req);
			VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, memType(&H, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
			VkDeviceMemory mem; CHECK(H.alloc(device, &mai, NULL, &mem)); CHECK(H.bindBuf(device, ibB, mem, 0));
		}
		/* uniforms: ViewProj maps x 0..8 -> -1..1, z 0..6 -> -1..1 (column-major) */
		float vp16[16] = { 0 }; vp16[0] = 2.0f / WCELLS_X; vp16[2 * 4 + 1] = 2.0f / WCELLS_Z; vp16[3 * 4 + 0] = -1.0f; vp16[3 * 4 + 1] = -1.0f; vp16[3 * 4 + 2] = 0.5f; vp16[3 * 4 + 3] = 1.0f;
		VkBuffer ubVp, ubDepth, ubWater;
		CHECK(hostBuffer(&H, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vp16, &ubVp, NULL));
		float depthU[64] = { 0 }; depthU[0] = WCELLS_X; depthU[1] = WCELLS_Z; depthU[2] = 100.0f /* _GameSeconds */; depthU[3] = 1.0f /* intensity */; depthU[4] = 1.0f /* ripple density */; depthU[5] = 0.0f /* no flow map */;
		CHECK(hostBuffer(&H, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, depthU, &ubDepth, NULL));
		float waterU[64] = { 0 };
		waterU[0] = waterU[1] = waterU[2] = waterU[3] = 1.0f;                                  /* _Color */
		waterU[4] = 0.5f; waterU[5] = 0.0f; waterU[6] = 0.5f; waterU[7] = 1.0f;                /* sun cast vector + intensity */
		waterU[8] = 0.0f; waterU[9] = 0.0f; waterU[10] = 0.0f; waterU[11] = 0.0f;              /* moon */
		waterU[12] = (float)extent.width; waterU[13] = (float)extent.height; waterU[14] = 1.0f / extent.width; waterU[15] = 1.0f / extent.height;
		waterU[16] = WCELLS_X; waterU[17] = WCELLS_Z; waterU[18] = 0.5f /* day */; waterU[19] = 100.0f /* seconds */; waterU[20] = 8.0f /* shine size reduction */; waterU[21] = 1.0f /* shine intensity */;
		CHECK(hostBuffer(&H, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, waterU, &ubWater, NULL));
		/* textures (uploaded in a setup command buffer) */
		VkCommandBufferBeginInfo sbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
		CHECK(vkBeginCommandBuffer(cmd, &sbi));
		VkImage timg[5]; VkImageView tview[5];
		CHECK(uploadTexture(&H, cmd, 64, 64, genRipple, &timg[0], &tview[0]));   /* _RippleTex */
		CHECK(uploadTexture(&H, cmd, 64, 64, genRamp, &timg[1], &tview[1]));     /* _MainTex (lighting ramp) */
		CHECK(uploadTexture(&H, cmd, 256, 256, genHash, &timg[2], &tview[2]));   /* _AlphaAddTex (edge noise) */
		CHECK(uploadTexture(&H, cmd, 256, 256, genSky, &timg[3], &tview[3]));    /* _WaterReflectionTex */
		CHECK(uploadTexture(&H, cmd, 64, 64, genGrey, &timg[4], &tview[4]));     /* _NoiseTex / _WaterOffsetTex */
		CHECK(vkEndCommandBuffer(cmd));
		VkSubmitInfo ssi = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL };
		CHECK(vkQueueSubmit(queue, 1, &ssi, VK_NULL_HANDLE)); CHECK(vkDeviceWaitIdle(device));
		/* the height-field target: R16_SFLOAT, screen sized, sampled by pass B */
		VkImage himg; VkImageView hview;
		CHECK(deviceImage(&H, VK_FORMAT_R16_SFLOAT, extent.width, extent.height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &himg, &hview));
		VkAttachmentDescription hatt = att; hatt.format = VK_FORMAT_R16_SFLOAT; hatt.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkRenderPassCreateInfo hrpci = rpci; hrpci.pAttachments = &hatt;
		CHECK(vkCreateRenderPass(device, &hrpci, NULL, &wrp));
		VkFramebufferCreateInfo hfci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, wrp, 1, &hview, extent.width, extent.height, 1 };
		CHECK(vkCreateFramebuffer(device, &hfci, NULL, &wfb));
		/* sampler like the game's: linear, repeat, anisotropic 2 */
		VkSamplerCreateInfo wsmci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
			VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0, VK_TRUE, 2.0f, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, VK_LOD_CLAMP_NONE, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_FALSE };
		VkSampler wsamp; CHECK(vkCreateSampler(device, &wsmci, NULL, &wsamp));
		/* descriptor set layouts: set 0 = ViewProj (VS); set 1 = empty (unused by these shaders); set 2 = material UBO + samplers (FS) */
		VkDescriptorSetLayoutBinding b0 = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
		VkDescriptorSetLayoutCreateInfo l0 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b0 };
		VkDescriptorSetLayoutCreateInfo l1 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL };
		VkDescriptorSetLayoutBinding bd[3] = { { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL }, { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
		VkDescriptorSetLayoutBinding bw[6] = { { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL }, { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
			{ 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL }, { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL }, { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
		VkDescriptorSetLayoutCreateInfo ld = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, bd };
		VkDescriptorSetLayoutCreateInfo lw = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 6, bw };
		VkDescriptorSetLayout dsl0, dsl1, dslD, dslW;
		CHECK(vkCreateDescriptorSetLayout(device, &l0, NULL, &dsl0)); CHECK(vkCreateDescriptorSetLayout(device, &l1, NULL, &dsl1));
		CHECK(vkCreateDescriptorSetLayout(device, &ld, NULL, &dslD)); CHECK(vkCreateDescriptorSetLayout(device, &lw, NULL, &dslW));
		VkPushConstantRange wpcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
		VkDescriptorSetLayout setsD[3] = { dsl0, dsl1, dslD }, setsW[3] = { dsl0, dsl1, dslW };
		VkPipelineLayoutCreateInfo plD = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 3, setsD, 1, &wpcr }, plW = plD; plW.pSetLayouts = setsW;
		CHECK(vkCreatePipelineLayout(device, &plD, NULL, &wdepthLayout)); CHECK(vkCreatePipelineLayout(device, &plW, NULL, &waterLayout));
		VkDescriptorPoolSize wps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 9 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 40 } };
		VkDescriptorPoolCreateInfo wdpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 9, 2, wps };
		VkDescriptorPool wpool; CHECK(vkCreateDescriptorPool(device, &wdpci, NULL, &wpool));
		VkDescriptorSetLayout alloc3[3] = { dsl0, dslD, dslW }; VkDescriptorSet got3[3];
		VkDescriptorSetAllocateInfo wdsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, wpool, 3, alloc3 };
		CHECK(vkAllocateDescriptorSets(device, &wdsai, got3)); wset0 = got3[0]; wsetDepth = got3[1]; wsetWater = got3[2];
		VkDescriptorBufferInfo dbVp = { ubVp, 0, 64 }, dbDepth = { ubDepth, 0, 32 }, dbWater = { ubWater, 0, 96 };
		VkDescriptorImageInfo diD[2] = { { wsamp, tview[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { wsamp, tview[4], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
		VkDescriptorImageInfo diW[5] = { { wsamp, tview[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { wsamp, tview[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { wsamp, hview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ wsamp, tview[3], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { wsamp, tview[4], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
		VkWriteDescriptorSet ww[5] = {
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wset0, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &dbVp, NULL },
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetDepth, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &dbDepth, NULL },
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetDepth, 1, 0, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, diD, NULL, NULL },
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetWater, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &dbWater, NULL },
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetWater, 1, 0, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, diW, NULL, NULL } };
		vkUpdateDescriptorSets(device, 5, ww, 0, NULL);
		if (ubowin) {
			float w2[128] = { 0 }; memcpy(w2, waterU, sizeof(float) * 64); memcpy(w2 + 64, waterU, sizeof(float) * 64); w2[64] = 1.0f; w2[65] = 0.3f; w2[66] = 0.3f;
			VkBuffer ubWater2; CHECK(hostBuffer(&H, 512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, w2, &ubWater2, NULL));
			VkDescriptorSetLayout alloc2[2] = { dslW, dslW }; VkDescriptorSet got2[2];
			VkDescriptorSetAllocateInfo a2 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, wpool, 2, alloc2 };
			CHECK(vkAllocateDescriptorSets(device, &a2, got2)); wsetU0 = got2[0]; wsetU1 = got2[1];
			VkDescriptorBufferInfo b0 = { ubWater2, 0, 96 }, b1 = { ubWater2, 256, 96 };
			VkWriteDescriptorSet wu[4] = {
				{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetU0, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &b0, NULL },
				{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetU0, 1, 0, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, diW, NULL, NULL },
				{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetU1, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &b1, NULL },
				{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetU1, 1, 0, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, diW, NULL, NULL } };
			vkUpdateDescriptorSets(device, 4, wu, 0, NULL);
		}
		if (setcopy) {
			VkDescriptorSetLayout alloc2[2] = { dslW, dslW }; VkDescriptorSet got2[2];
			VkDescriptorSetAllocateInfo a2 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, wpool, 2, alloc2 };
			CHECK(vkAllocateDescriptorSets(device, &a2, got2)); wsetCopy = got2[0]; wsetOther = got2[1];
			VkDescriptorImageInfo diO[5] = { diW[1], diW[0], diW[2], diW[4], diW[3] };   /* textures swapped: different content */
			VkWriteDescriptorSet wc[4] = {
				{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetCopy, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &dbWater, NULL },
				{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetCopy, 1, 0, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, diW, NULL, NULL },
				{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetOther, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &dbWater, NULL },
				{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, wsetOther, 1, 0, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, diO, NULL, NULL } };
			vkUpdateDescriptorSets(device, 4, wc, 0, NULL);
		}
		/* pipelines: position (binding 0, loc 0) + colour (binding 1, loc 2); alpha blending like the game */
		VkShaderModuleCreateInfo m1 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(watervs_spv), watervs_spv };
		VkShaderModuleCreateInfo m2 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(wdepthfs_spv), wdepthfs_spv };
		VkShaderModuleCreateInfo m3 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(waterfs_spv), waterfs_spv };
		VkShaderModule wvs, dfs, wfs; CHECK(vkCreateShaderModule(device, &m1, NULL, &wvs)); CHECK(vkCreateShaderModule(device, &m2, NULL, &dfs)); CHECK(vkCreateShaderModule(device, &m3, NULL, &wfs));
		VkVertexInputBindingDescription wbind[2] = { { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 16, VK_VERTEX_INPUT_RATE_INSTANCE } };
		VkVertexInputAttributeDescription wattr[2] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 12 } };
		VkPipelineVertexInputStateCreateInfo wvi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 2, wbind, 2, wattr };
		VkPipelineColorBlendAttachmentState wcba = { VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xF };
		VkPipelineColorBlendStateCreateInfo wcb = cb; wcb.pAttachments = &wcba;
		VkPipelineShaderStageCreateInfo dst[2] = { stages[0], stages[1] }; dst[0].module = wvs; dst[1].module = dfs;
		VkPipelineShaderStageCreateInfo wst[2] = { stages[0], stages[1] }; wst[0].module = wvs; wst[1].module = wfs;
		VkGraphicsPipelineCreateInfo dgp = gpci; dgp.pStages = dst; dgp.pVertexInputState = &wvi; dgp.pColorBlendState = &wcb; dgp.layout = wdepthLayout; dgp.renderPass = wrp;
		VkGraphicsPipelineCreateInfo wgp = dgp; wgp.pStages = wst; wgp.layout = waterLayout; wgp.renderPass = rp;
		CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &dgp, NULL, &wdepthPipe));
		CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &wgp, NULL, &waterPipe));
		printf("water mode: WaterDepth pass (R16_SFLOAT height field, %d cells) then TerrainWater to the swapchain, game shaders; expect a rippled, non-uniform surface\n", WCELLS_X * WCELLS_Z);
	}

	/* v13.43 instuav: the vertex shader reads per-instance offsets from a storage buffer that is
	   not declared readonly (the bridge binds it as a graphics UAV). Draw 1 (offset -0.5) ->
	   vkCmdClearAttachments on a small corner rectangle (the bridge does this with a helper draw
	   that switches render targets) -> the SAME descriptor set bound again -> draw 2 with a push
	   constant shift of +1.0. Correct: two blue triangles at x = -0.5 and +0.5. Bug: the UAV was
	   dropped by the helper pass, draw 2 reads offset 0 and lands at x = +1.0 (half off screen). */
	VkPipeline instPipe = VK_NULL_HANDLE; VkPipelineLayout instLayout = VK_NULL_HANDLE; VkDescriptorSet instSet = VK_NULL_HANDLE;
	if (instuav && !uavlayout)
	{
		/* uavoffset: junk (+5, off screen) at offset 0, the real offsets at byte 256; the descriptor points at 256 */
		float offs[128] = { -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		if (uavoffset) { for (int k = 0; k < 64; ++k) offs[k] = 5.0f; offs[64] = -0.5f; offs[65] = 0.0f; offs[66] = 0.0f; offs[67] = 0.0f; for (int k = 68; k < 128; ++k) offs[k] = 0.0f; }
		/* uavalias: offsets at byte 0 (instance 0 = -0.5), colours at byte 256 (instance 0 = opaque blue) */
		if (uavalias) { for (int k = 64; k < 128; ++k) offs[k] = 0.0f; offs[64] = 0.0f; offs[65] = 0.0f; offs[66] = 1.0f; offs[67] = 1.0f; }
		VkBuffer ibuf; CHECK(hostBuffer(&H, 512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, offs, &ibuf, NULL));
		VkDescriptorSetLayoutBinding ib[2] = { { 0, uavdyn ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL } };
		VkDescriptorSetLayoutCreateInfo idl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, uavalias ? 2u : 1u, ib };
		VkDescriptorSetLayout idsl; CHECK(vkCreateDescriptorSetLayout(device, &idl, NULL, &idsl));
		VkPushConstantRange ipcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
		VkPipelineLayoutCreateInfo ipl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &idsl, 1, &ipcr };
		CHECK(vkCreatePipelineLayout(device, &ipl, NULL, &instLayout));
		VkDescriptorPoolSize ips = { uavdyn ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
		VkDescriptorPoolCreateInfo idp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ips };
		VkDescriptorPool ipool; CHECK(vkCreateDescriptorPool(device, &idp, NULL, &ipool));
		VkDescriptorSetAllocateInfo ida = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, ipool, 1, &idsl };
		CHECK(vkAllocateDescriptorSets(device, &ida, &instSet));
		VkDescriptorBufferInfo idbi = { ibuf, (uavoffset && !uavdyn) ? 256 : 0, 256 };
		VkWriteDescriptorSet iw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, instSet, 0, 0, 1, uavdyn ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &idbi, NULL };
		vkUpdateDescriptorSets(device, 1, &iw, 0, NULL);
		if (uavalias)
		{
			VkDescriptorBufferInfo cdbi = { ibuf, 256, 256 };
			VkWriteDescriptorSet cw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, instSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &cdbi, NULL };
			vkUpdateDescriptorSets(device, 1, &cw, 0, NULL);
		}
		VkShaderModuleCreateInfo m1 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(instvs_spv), instvs_spv };
		VkShaderModuleCreateInfo m2 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(instfs_spv), instfs_spv };
		if (uavalias) { m1.codeSize = sizeof(inst3vs_spv); m1.pCode = inst3vs_spv; m2.codeSize = sizeof(inst3fs_spv); m2.pCode = inst3fs_spv; }
		VkShaderModule ivs, ifs; CHECK(vkCreateShaderModule(device, &m1, NULL, &ivs)); CHECK(vkCreateShaderModule(device, &m2, NULL, &ifs));
		VkPipelineShaderStageCreateInfo ist[2] = { stages[0], stages[1] }; ist[0].module = ivs; ist[1].module = ifs;
		VkGraphicsPipelineCreateInfo igp = gpci; igp.pStages = ist; igp.layout = instLayout;
		CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &igp, NULL, &instPipe));
		printf("instuav mode: writable SSBO offsets in the VS, draw / clearAttachments / rebind / draw; expect TWO blue triangles (left and right)\n");
	}
	/* v13.47 uavlayout: pipeline layout LB = { set0: one uniform buffer, set1: instance SSBO }.
	   The sets are bound with LA = { set0: one storage buffer (junk offsets +5, off screen), set1: same
	   instance layout }. Shader register for set1.b0 comes from LB (UAV index 0); binding with LA
	   gave index 1 and the junk buffer sat at index 0 -> triangles vanish. Correct: two blue triangles. */
	VkPipelineLayout ulA = VK_NULL_HANDLE; VkDescriptorSet uJunk = VK_NULL_HANDLE, uInst = VK_NULL_HANDLE;
	if (uavlayout)
	{
		float junk[8] = { 5.0f, 5.0f, 0, 0, 5.0f, 5.0f, 0, 0 };
		float offs[8] = { -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		VkBuffer jbuf, obuf, ubuf;
		CHECK(hostBuffer(&H, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, junk, &jbuf, NULL));
		CHECK(hostBuffer(&H, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, offs, &obuf, NULL));
		CHECK(hostBuffer(&H, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, junk, &ubuf, NULL));
		VkDescriptorSetLayoutBinding sb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
		VkDescriptorSetLayoutBinding ub = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
		VkDescriptorSetLayoutCreateInfo sdl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &sb };
		VkDescriptorSetLayoutCreateInfo udl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &ub };
		VkDescriptorSetLayout sJunkL, sUboL, sInstL;
		CHECK(vkCreateDescriptorSetLayout(device, &sdl, NULL, &sJunkL));
		CHECK(vkCreateDescriptorSetLayout(device, &udl, NULL, &sUboL));
		CHECK(vkCreateDescriptorSetLayout(device, &sdl, NULL, &sInstL));
		VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
		VkDescriptorSetLayout la[2] = { sJunkL, sInstL }, lb[2] = { sUboL, sInstL };
		VkPipelineLayoutCreateInfo plA = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, la, 1, &pcr };
		VkPipelineLayoutCreateInfo plB = plA; plB.pSetLayouts = lb;
		CHECK(vkCreatePipelineLayout(device, &plA, NULL, &ulA));
		CHECK(vkCreatePipelineLayout(device, &plB, NULL, &instLayout));
		VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
		VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 2, ps };
		VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dp, NULL, &pool));
		VkDescriptorSetLayout al[2] = { sJunkL, sInstL }; VkDescriptorSet sets[2];
		VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 2, al };
		CHECK(vkAllocateDescriptorSets(device, &da, sets)); uJunk = sets[0]; uInst = sets[1];
		VkDescriptorBufferInfo jbi = { jbuf, 0, 256 }, obi = { obuf, 0, 256 };
		VkWriteDescriptorSet w[2] = {
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, uJunk, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &jbi, NULL },
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, uInst, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &obi, NULL } };
		vkUpdateDescriptorSets(device, 2, w, 0, NULL);
		(void)ubuf;
		VkShaderModuleCreateInfo m1 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(inst2vs_spv), inst2vs_spv };
		VkShaderModuleCreateInfo m2 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(instfs_spv), instfs_spv };
		VkShaderModule ivs, ifs; CHECK(vkCreateShaderModule(device, &m1, NULL, &ivs)); CHECK(vkCreateShaderModule(device, &m2, NULL, &ifs));
		VkPipelineShaderStageCreateInfo ist[2] = { stages[0], stages[1] }; ist[0].module = ivs; ist[1].module = ifs;
		VkGraphicsPipelineCreateInfo igp = gpci; igp.pStages = ist; igp.layout = instLayout;
		CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &igp, NULL, &instPipe));
		printf("uavlayout mode: instance SSBO in set 1, sets bound with a sibling layout whose set 0 holds a junk SSBO; expect TWO blue triangles (left and right)\n");
	}
	/* v13.41: CPU readback buffer for the last frame */
	VkBuffer rbBuf = VK_NULL_HANDLE; unsigned char * rbData = NULL;
	if (readback) CHECK(hostBuffer(&H, (VkDeviceSize)extent.width * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL, &rbBuf, (void **)&rbData));
	VkSemaphoreCreateInfo semci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkSemaphore acquired; CHECK(vkCreateSemaphore(device, &semci, NULL, &acquired));

	for (int f = 1; f <= frames; ++f)
	{
		if (dynData) { /* element at offset 512: magenta in frame 1, cyan from frame 2 (a stale copy shows magenta) */
			dynData[128] = f >= 2 ? 0.0f : 1.0f; dynData[129] = f >= 2 ? 1.0f : 0.0f; dynData[130] = 1.0f; dynData[131] = 1.0f;
			if (getenv("SMOKE_MULTIWIN")) {   /* v13.73: three windows, new colours every frame (a stale merged upload shows old colours) */
				float const k = (float)(f % 3) * 0.5f;
				dynData[0] = k; dynData[1] = 1.0f - k; dynData[2] = 0.25f; dynData[3] = 1.0f;
				dynData[192] = 1.0f - k; dynData[193] = 0.25f; dynData[194] = k; dynData[195] = 1.0f; } }
		if (idxmulti) {   /* v13.74: frame 1 wrong indices (shifted by one), later frames the right ones */
			for (int i = 0; i < WINDICES / 2; ++i) { ibAMap[i] = f == 1 ? (idxKeep[i] + 1) % (WCELLS_X * WCELLS_Z * 4) : idxKeep[i]; ibBStageMap[i] = f == 1 ? (idxKeep[WINDICES / 2 + i] + 1) % (WCELLS_X * WCELLS_Z * 4) : idxKeep[WINDICES / 2 + i]; } }
		uint32_t index = 0;
		CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index));
		CHECK(vkResetCommandBuffer(cmd, 0));
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		CHECK(vkBeginCommandBuffer(cmd, &bi));
		VkClearValue clear; clear.color.float32[0] = 1.0f; clear.color.float32[1] = 0.0f; clear.color.float32[2] = 0.0f; clear.color.float32[3] = 1.0f;
		if (hdr)
		{
			VkClearValue oclear; oclear.color.float32[0] = 1.0f; oclear.color.float32[1] = 0.0f; oclear.color.float32[2] = 0.0f; oclear.color.float32[3] = 1.0f;
			VkRenderPassBeginInfo orpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, orp, ofb, { { 0, 0 }, extent }, 1, &oclear };
			vkCmdBeginRenderPass(cmd, &orpbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opipe);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
			VkImageMemoryBarrier tb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[index], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
			VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int)extent.width, (int)extent.height, 1 } }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int)extent.width, (int)extent.height, 1 } } };
			vkCmdBlitImage(cmd, hdrimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
			tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; tb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; tb.dstAccessMask = 0;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
		}
		else if (mipblit)
		{
			for (int i = 0; i < NMIP; ++i)
			{
				VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, mipimg[i], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1 } };
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
				VkClearColorValue cc; cc.float32[0] = 0.5f; cc.float32[1] = 0.5f; cc.float32[2] = 0.5f; cc.float32[3] = 1.0f;
				VkImageSubresourceRange r0 = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkCmdClearColorImage(cmd, mipimg[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &r0);
				VkImageMemoryBarrier sb = mb; sb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; sb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; sb.subresourceRange.levelCount = 1;
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &sb);
				VkImageBlit mbl = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { 64, 64, 1 } }, { VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1 }, { { 0, 0, 0 }, { 32, 32, 1 } } };
				vkCmdBlitImage(cmd, mipimg[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mipimg[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mbl, VK_FILTER_LINEAR);
			}
			VkImageMemoryBarrier m1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, mipimg[0], { VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, 1 } };
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &m1);
			VkImageMemoryBarrier tb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[index], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
			VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)mipsrc, 0, 1 }, { { 0, 0, 0 }, { mipsrc ? 32 : 64, mipsrc ? 32 : 64, 1 } }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int)extent.width, (int)extent.height, 1 } } };
			vkCmdBlitImage(cmd, mipimg[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
			tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; tb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; tb.dstAccessMask = 0;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
		}
		else if (composite)
		{
			VkClearValue oclear; oclear.color.float32[0] = 0.2f; oclear.color.float32[1] = 0.2f; oclear.color.float32[2] = 0.2f; oclear.color.float32[3] = 1.0f;
			VkRenderPassBeginInfo orpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, orp, ofb, { { 0, 0 }, extent }, 1, &oclear };
			vkCmdBeginRenderPass(cmd, &orpbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opipe);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
		}
		else if (water)
		{
			VkClearValue hclear; memset(&hclear, 0, sizeof(hclear));
			VkRenderPassBeginInfo hrpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, wrp, wfb, { { 0, 0 }, extent }, 1, &hclear };
			VkBuffer wvbs[2] = { wvbPos, wvbCol }; VkDeviceSize woffs[2] = { 0, 0 };
			float model[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
			if (idxmulti) {
				VkBufferCopy bc = { 0, 0, sizeof(idxKeep) / 2 }; vkCmdCopyBuffer(cmd, ibBStage, ibB, 1, &bc);
				VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDEX_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ibB, 0, VK_WHOLE_SIZE };
				H.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
			}
			for (int pass = 0; pass < 2; ++pass)
			{
				vkCmdBeginRenderPass(cmd, pass == 0 ? &hrpbi : &(VkRenderPassBeginInfo){ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fbs[index], { { 0, 0 }, extent }, 1, &clear }, VK_SUBPASS_CONTENTS_INLINE);
				VkPipelineLayout pl = pass == 0 ? wdepthLayout : waterLayout;
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? wdepthPipe : waterPipe);
				vkCmdBindVertexBuffers(cmd, 0, 2, wvbs, woffs);
				vkCmdBindIndexBuffer(cmd, wib, ibofs ? WPAD * 4 : 0, VK_INDEX_TYPE_UINT32);
				vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, model);
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &wset0, 0, NULL);
				VkDescriptorSet s2 = pass == 0 ? wsetDepth : wsetWater;
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 2, 1, &s2, 0, NULL);
				if (ubowin && pass == 1)
				{
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 2, 1, &wsetU0, 0, NULL);
					vkCmdDrawIndexed(cmd, WINDICES / 2, 1, 0, 0, 0);
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 2, 1, &wsetU1, 0, NULL);
					vkCmdDrawIndexed(cmd, WINDICES / 2, 1, WINDICES / 2, 0, 0);
				}
				else if (idxmulti && pass == 1)
				{
					vkCmdBindIndexBuffer(cmd, ibA, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd, WINDICES / 2, 1, 0, 0, 0);
					vkCmdBindIndexBuffer(cmd, ibB, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cmd, WINDICES / 2, 1, 0, 0, 0);
				}
				else if (setcopy && pass == 1)
				{
					/* =2: first half with the original set, second half with a new handle of identical content
					   =3: as 2, but a set with different textures is bound in between (no draw): the copy must
					       be bound in full again. Both must equal the normal water picture. */
					vkCmdDrawIndexed(cmd, WINDICES / 2, 1, 0, 0, 0);
					if (getenv("SMOKE_SETCOPY")[0] == '3') vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 2, 1, &wsetOther, 0, NULL);
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 2, 1, getenv("SMOKE_SETCOPY")[0] == '4' ? &wsetOther : &wsetCopy, 0, NULL);   /* =4: different textures, picture must differ */
					vkCmdDrawIndexed(cmd, WINDICES / 2, 1, WINDICES / 2, 0, 0);
				}
				else if (ibofs)
				{
					vkCmdDrawIndexed(cmd, WINDICES / 2 - 6, 1, 6, 0, 0);   /* first half minus the first cell, firstIndex 6 */
					vkCmdBindIndexBuffer(cmd, wib, WPAD * 4 + 0, VK_INDEX_TYPE_UINT32);
					vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);                  /* the first cell */
					vkCmdBindIndexBuffer(cmd, wib, getenv("SMOKE_IBOFS")[0] == '3' ? 0 : (WPAD + WINDICES / 2) * 4, VK_INDEX_TYPE_UINT32);
					if (getenv("SMOKE_IBOFS")[0] == '2') vkCmdDrawIndexed(cmd, WINDICES / 2, 1, 0, 0, 0); else
					vkCmdDrawIndexedIndirect(cmd, wind, 0, 1, 20);         /* second half from the GPU buffer */
				}
				else
				vkCmdDrawIndexed(cmd, WINDICES, 1, 0, 0, 0);
				vkCmdEndRenderPass(cmd);
			}
		}
		else if (instuav)
		{
			VkRenderPassBeginInfo irp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fbs[index], { { 0, 0 }, extent }, 1, &clear };
			vkCmdBeginRenderPass(cmd, &irp, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, instPipe);
			float sh0[4] = { 0, 0, 0, 0 }, sh1[4] = { 1.0f, 0, 0, 0 };
			if (uavlayout) { VkDescriptorSet bs[2] = { uJunk, uInst }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ulA, 0, 2, bs, 0, NULL); }
			else { uint32_t dynOff = 256; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, instLayout, 0, 1, &instSet, uavdyn ? 1 : 0, uavdyn ? &dynOff : NULL); }
			vkCmdPushConstants(cmd, instLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, sh0);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			VkClearAttachment ica = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { 0 } }; ica.clearValue.color.float32[3] = 1.0f;
			VkClearRect icr = { { { 0, 0 }, { 8, 8 } }, 0, 1 };
			vkCmdClearAttachments(cmd, 1, &ica, 1, &icr);
			if (uavlayout) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ulA, 1, 1, &uInst, 0, NULL);
			else { uint32_t dynOff = 256; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, instLayout, 0, 1, &instSet, uavdyn ? 1 : 0, uavdyn ? &dynOff : NULL); }
			vkCmdPushConstants(cmd, instLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, sh1);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
		}
		VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fbs[index], { { 0, 0 }, extent }, 1, &clear };
		if (hdr || mipblit || water || instuav) goto submit;
		vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
		if (composite)
		{
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cpipe);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, clayout, 0, 1, &dset, 0, NULL);
		}
		else
		{
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			if (pushc)
			{
				float scale[2] = { 2.0f / (float)extent.width, 2.0f / (float)extent.height };
				float offset[2] = { -1.0f, -1.0f };
				vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, scale);
				vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 8, 8, offset);
			}
		}
		if (ssbo) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ssboSet, 0, NULL); vkCmdDraw(cmd, 3, 4, 0, 0); }
		else if (pushubo) { float off[4] = { 0.5f, 0.0f, 0.0f, 0.0f }; vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, off); { uint32_t dynOff = 512; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &pubSet, dynubo ? 1u : 0u, dynubo ? &dynOff : NULL); } vkCmdDraw(cmd, 3, 1, 0, 0); /* push BEFORE bind, as Waylith does per draw */
			if (dynubo && getenv("SMOKE_MULTIWIN")) {   /* v13.73: two more triangles from windows at 0 and 768 of the same buffer */
				uint32_t const offs[2] = { 0, 768 }; float const xs[2] = { -0.9f, -0.2f };
				for (int t = 0; t < 2; ++t) { float o2[4] = { xs[t], 0.0f, 0.0f, 0.0f }; vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, o2);
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &pubSet, 1u, &offs[t]); vkCmdDraw(cmd, 3, 1, 0, 0); } } }
		else vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
		if (badblit)
		{
			/* deliberately invalid: negative (flipped) blit rectangle -> bridge must refuse and the watchdog must report */
			VkImageBlit bb = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { (int)extent.width, (int)extent.height, 0 }, { 0, 0, 1 } }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int)extent.width, (int)extent.height, 1 } } };
			vkCmdBlitImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bb, VK_FILTER_NEAREST);
		}
		if (clearatt)
		{
			/* world-pass pattern: clear colour (blue) + depth over the LEFT half only, then draw */
			VkRenderPassBeginInfo rpbi3 = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp2, fbs2[index], { { 0, 0 }, extent }, 0, NULL };
			vkCmdBeginRenderPass(cmd, &rpbi3, VK_SUBPASS_CONTENTS_INLINE);
			VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { 0 } };
			ca.clearValue.color.float32[2] = 1.0f; ca.clearValue.color.float32[3] = 1.0f;
			VkClearRect cr = { { { 0, 0 }, { extent.width / 2, extent.height } }, 0, 1 };
			vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
		}
		if (areaclear)
		{
			/* pawn-atlas pattern: a CLEAR render pass whose render area is only part of the
			   framebuffer must leave the rest untouched. Expect: left half blue, right half
			   keeps the red background and the right half of the green triangle. */
			VkClearValue blue; blue.color.float32[0] = 0.0f; blue.color.float32[1] = 0.0f; blue.color.float32[2] = 1.0f; blue.color.float32[3] = 1.0f;
			VkRenderPassBeginInfo rpbi4 = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fbs[index], { { 0, 0 }, { extent.width / 2, extent.height } }, 1, &blue };
			vkCmdBeginRenderPass(cmd, &rpbi4, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdEndRenderPass(cmd);
		}
		if (vbopersist)
		{
			VkBuffer vbs[2] = { vboPos, vboCol }; VkDeviceSize voffs[2] = { 0, 48 };
			vkCmdBindVertexBuffers(cmd, 0, 2, vbs, voffs);            /* 1. bind the vertex buffers */
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline); /* 2. a pipeline WITHOUT vertex input */
			VkRenderPassBeginInfo rpbi5 = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp2, fbs2[index], { { 0, 0 }, extent }, 0, NULL };
			vkCmdBeginRenderPass(cmd, &rpbi5, VK_SUBPASS_CONTENTS_INLINE); /* 3. a render pass */
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vbopipe); /* 4. the pipeline that uses them */
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
		}
		if (loadpass)
		{
			VkRenderPassBeginInfo rpbi2 = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp2, fbs2[index], { { 0, 0 }, extent }, 0, NULL };
			vkCmdBeginRenderPass(cmd, &rpbi2, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
		}
		submit:
		if (readback && f == frames)
		{
			/* v13.41: copy the presented image back to the CPU (works on any ICD, so the same
			   numbers can be produced by a reference driver and compared) */
			VkImageMemoryBarrier rb1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[index], { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &rb1);
			VkBufferImageCopy rbc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { extent.width, extent.height, 1 } };
			vkCmdCopyImageToBuffer(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbBuf, 1, &rbc);
			VkImageMemoryBarrier rb2 = rb1; rb2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; rb2.dstAccessMask = 0; rb2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; rb2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &rb2);
		}
		CHECK(vkEndCommandBuffer(cmd));
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired, &stage, 1, &cmd, 0, NULL };
		CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
		VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &swapchain, &index, NULL };
		CHECK(vkQueuePresentKHR(queue, &pi));
		CHECK(vkDeviceWaitIdle(device));
		printf("frame %d presented image %u\n", f, index);
		if (readback && f == frames)
		{
			/* mean colour, luminance spread and a 8x6 grid of samples (BGRA swapchain) */
			double sr = 0, sg = 0, sb = 0, sl = 0, sl2 = 0; size_t n = (size_t)extent.width * extent.height;
			for (size_t i = 0; i < n; ++i) { unsigned char * p = rbData + i * 4; double l = (p[2] * 0.299 + p[1] * 0.587 + p[0] * 0.114); sr += p[2]; sg += p[1]; sb += p[0]; sl += l; sl2 += l * l; }
			double ml = sl / n, var = sl2 / n - ml * ml;
			printf("readback frame=%d size=%ux%u meanRGB=%.1f,%.1f,%.1f lumaStddev=%.1f\n", f, extent.width, extent.height, sr / n, sg / n, sb / n, var > 0 ? sqrt(var) : 0.0);
			printf("readback grid:");
			for (int gy = 0; gy < 6; ++gy) for (int gx = 0; gx < 8; ++gx)
			{
				uint32_t x = (uint32_t)((gx + 0.5) / 8.0 * extent.width), y = (uint32_t)((gy + 0.5) / 6.0 * extent.height);
				unsigned char * p = rbData + ((size_t)y * extent.width + x) * 4;
				printf(" %u,%u,%u", p[2], p[1], p[0]);
			}
			printf("\n");
		}
	}
	vkDestroySwapchainKHR(device, swapchain, NULL);
	printf("PASS: %d frames\n", frames);
	return 0;
}
