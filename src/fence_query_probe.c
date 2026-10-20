/* Focused synthetic validation for D3D11 EVENT-backed Vulkan fences and queries.
 * The loader and ICD are explicit absolute paths so this test does not modify a
 * system Vulkan installation.  It permits Wine's synthetic adapter only. */
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fflush(stderr); failed = 1; goto cleanup; } while (0)
#define LOAD_I(i,n) p_##n=(PFN_##n)gipa((i),#n)
#define LOAD_D(d,n) p_##n=(PFN_##n)p_vkGetDeviceProcAddr((d),#n)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkDestroyInstance p_vkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkDestroyDevice p_vkDestroyDevice;
static PFN_vkGetDeviceQueue p_vkGetDeviceQueue;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkDestroyFence p_vkDestroyFence;
static PFN_vkGetFenceStatus p_vkGetFenceStatus;
static PFN_vkResetFences p_vkResetFences;
static PFN_vkWaitForFences p_vkWaitForFences;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkQueueWaitIdle p_vkQueueWaitIdle;
static PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle;
static PFN_vkCreateCommandPool p_vkCreateCommandPool;
static PFN_vkDestroyCommandPool p_vkDestroyCommandPool;
static PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
static PFN_vkCmdFillBuffer p_vkCmdFillBuffer;
static PFN_vkCmdWriteTimestamp p_vkCmdWriteTimestamp;
static PFN_vkCreateQueryPool p_vkCreateQueryPool;
static PFN_vkDestroyQueryPool p_vkDestroyQueryPool;
static PFN_vkGetQueryPoolResults p_vkGetQueryPoolResults;
static PFN_vkCreateBuffer p_vkCreateBuffer;
static PFN_vkDestroyBuffer p_vkDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindBufferMemory p_vkBindBufferMemory;
static PFN_vkMapMemory p_vkMapMemory;
static PFN_vkUnmapMemory p_vkUnmapMemory;

static const char *name_result(VkResult r) {
    switch(r) { case VK_SUCCESS:return "VK_SUCCESS"; case VK_NOT_READY:return "VK_NOT_READY";
    case VK_TIMEOUT:return "VK_TIMEOUT"; case VK_ERROR_DEVICE_LOST:return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED:return "VK_ERROR_INITIALIZATION_FAILED"; default:return "OTHER"; }
}
static int is_abs(const wchar_t *p) { return p && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'); }
static int fallback_enabled(void) { char v[2]={0}; return GetEnvironmentVariableA("RW_HD5500_FENCE_EVENTS",v,2)==1 && v[0]=='0'; }
static int host_type(uint32_t bits, const VkPhysicalDeviceMemoryProperties *mp) {
    uint32_t i; for(i=0;i<mp->memoryTypeCount;i++) if((bits&(1u<<i)) &&
      (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return (int)i;
    return -1;
}
static int require_ptr(void *p,const char *name) { if(!p) { fprintf(stderr,"FAIL: missing %s\n",name); return 0; } return 1; }

int wmain(int argc, wchar_t **argv) {
    const wchar_t *loader=0,*icd=0; HMODULE mod=0; int i,failed=0,fb=fallback_enabled();
    VkInstance instance=VK_NULL_HANDLE; VkPhysicalDevice physical=VK_NULL_HANDLE,*list=0; uint32_t count=0,family=UINT32_MAX;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties memprops; VkDevice device=VK_NULL_HANDLE; VkQueue queue=VK_NULL_HANDLE;
    VkFence f1=VK_NULL_HANDLE,f2=VK_NULL_HANDLE,fs=VK_NULL_HANDLE; VkCommandPool pool=VK_NULL_HANDLE; VkCommandBuffer cmd=VK_NULL_HANDLE;
    VkQueryPool queries=VK_NULL_HANDLE; VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; int mapped=0;
    for(i=1;i<argc;i++) { if(!wcscmp(argv[i],L"--loader")&&i+1<argc)loader=argv[++i]; else if(!wcscmp(argv[i],L"--icd")&&i+1<argc)icd=argv[++i]; }
    if(!is_abs(loader)||!is_abs(icd)) { fprintf(stderr,"FAIL: absolute --loader and --icd are required\n"); return 2; }
    SetEnvironmentVariableW(L"VK_ICD_FILENAMES",icd); SetEnvironmentVariableW(L"VK_DRIVER_FILES",icd);
    mod=LoadLibraryW(loader); if(!mod) { fprintf(stderr,"FAIL: LoadLibraryW loader\n"); return 3; }
    gipa=(PFN_vkGetInstanceProcAddr)GetProcAddress(mod,"vkGetInstanceProcAddr"); if(!gipa) FAIL("no vkGetInstanceProcAddr\n");
    p_vkCreateInstance=(PFN_vkCreateInstance)gipa(0,"vkCreateInstance"); if(!p_vkCreateInstance) FAIL("no vkCreateInstance\n");
    { VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO,0,"FenceQueryProbe",1,"probe",1,VK_API_VERSION_1_0};
      VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,0,0,&ai,0,0,0,0};
      if(p_vkCreateInstance(&ci,0,&instance)!=VK_SUCCESS) FAIL("vkCreateInstance\n"); }
    LOAD_I(instance,vkDestroyInstance); LOAD_I(instance,vkEnumeratePhysicalDevices); LOAD_I(instance,vkGetPhysicalDeviceProperties); LOAD_I(instance,vkGetPhysicalDeviceMemoryProperties); LOAD_I(instance,vkGetPhysicalDeviceQueueFamilyProperties); LOAD_I(instance,vkCreateDevice); LOAD_I(instance,vkGetDeviceProcAddr);
    if(!p_vkDestroyInstance||!p_vkEnumeratePhysicalDevices||!p_vkGetPhysicalDeviceProperties||!p_vkGetPhysicalDeviceMemoryProperties||!p_vkGetPhysicalDeviceQueueFamilyProperties||!p_vkCreateDevice||!p_vkGetDeviceProcAddr) FAIL("missing instance entrypoint\n");
    if(p_vkEnumeratePhysicalDevices(instance,&count,0)!=VK_SUCCESS||!count) FAIL("no physical devices\n");
    list=(VkPhysicalDevice*)calloc(count,sizeof(*list)); if(!list||p_vkEnumeratePhysicalDevices(instance,&count,list)!=VK_SUCCESS) FAIL("physical enumeration\n");
    physical=list[0]; p_vkGetPhysicalDeviceProperties(physical,&props); p_vkGetPhysicalDeviceMemoryProperties(physical,&memprops);
    { uint32_t n=0; VkQueueFamilyProperties *q; p_vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,0); q=(VkQueueFamilyProperties*)calloc(n,sizeof(*q)); if(!q) FAIL("queue alloc\n"); p_vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,q); for(i=0;i<(int)n;i++)if(q[i].queueCount&&(q[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=(uint32_t)i;break;} free(q); if(family==UINT32_MAX)FAIL("no graphics queue\n"); }
    LOG("ADAPTER: %s vendor=0x%04X timestampPeriod=%.9f fallback=%d\n",props.deviceName,props.vendorID,props.limits.timestampPeriod,fb);
    { float priority=1.0f; VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,0,0,family,1,&priority}; VkDeviceCreateInfo di={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,0,0,1,&qi,0,0,0,0,0}; if(p_vkCreateDevice(physical,&di,0,&device)!=VK_SUCCESS)FAIL("vkCreateDevice\n"); }
    LOAD_D(device,vkDestroyDevice); LOAD_D(device,vkGetDeviceQueue); LOAD_D(device,vkCreateFence); LOAD_D(device,vkDestroyFence); LOAD_D(device,vkGetFenceStatus); LOAD_D(device,vkResetFences); LOAD_D(device,vkWaitForFences); LOAD_D(device,vkQueueSubmit); LOAD_D(device,vkQueueWaitIdle); LOAD_D(device,vkDeviceWaitIdle); LOAD_D(device,vkCreateCommandPool); LOAD_D(device,vkDestroyCommandPool); LOAD_D(device,vkAllocateCommandBuffers); LOAD_D(device,vkBeginCommandBuffer); LOAD_D(device,vkEndCommandBuffer); LOAD_D(device,vkCmdFillBuffer); LOAD_D(device,vkCmdWriteTimestamp); LOAD_D(device,vkCreateQueryPool); LOAD_D(device,vkDestroyQueryPool); LOAD_D(device,vkGetQueryPoolResults); LOAD_D(device,vkCreateBuffer); LOAD_D(device,vkDestroyBuffer); LOAD_D(device,vkGetBufferMemoryRequirements); LOAD_D(device,vkAllocateMemory); LOAD_D(device,vkFreeMemory); LOAD_D(device,vkBindBufferMemory); LOAD_D(device,vkMapMemory); LOAD_D(device,vkUnmapMemory);
    if(!require_ptr(p_vkGetFenceStatus,"vkGetFenceStatus")||!require_ptr(p_vkCmdWriteTimestamp,"vkCmdWriteTimestamp")||!require_ptr(p_vkGetQueryPoolResults,"vkGetQueryPoolResults")||!require_ptr(p_vkCmdFillBuffer,"vkCmdFillBuffer")) {failed=1;goto cleanup;}
    p_vkGetDeviceQueue(device,family,0,&queue); if(!queue)FAIL("no queue\n");
    { VkFenceCreateInfo ci={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,0,0}; VkFenceCreateInfo cs={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,0,VK_FENCE_CREATE_SIGNALED_BIT}; if(p_vkCreateFence(device,&ci,0,&f1)||p_vkCreateFence(device,&ci,0,&f2)||p_vkCreateFence(device,&cs,0,&fs))FAIL("fence create\n"); }
    { VkResult r=p_vkGetFenceStatus(device,f1); LOG("FENCE_UNSIGNALED_STATUS: %s\n",name_result(r)); if(!fb&&r!=VK_NOT_READY)FAIL("unsignaled fence must be NOT_READY\n"); if(p_vkGetFenceStatus(device,fs)!=VK_SUCCESS)FAIL("created-signaled fence\n"); }
    { VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,0,0,4096,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_SHARING_MODE_EXCLUSIVE,0,0}; VkMemoryRequirements mr; VkMemoryAllocateInfo mi; int mt; if(p_vkCreateBuffer(device,&bi,0,&buffer)!=VK_SUCCESS)FAIL("buffer create\n"); p_vkGetBufferMemoryRequirements(device,buffer,&mr); mt=host_type(mr.memoryTypeBits,&memprops); if(mt<0)FAIL("host-visible buffer memory unavailable\n"); memset(&mi,0,sizeof(mi)); mi.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mi.allocationSize=mr.size;mi.memoryTypeIndex=(uint32_t)mt; if(p_vkAllocateMemory(device,&mi,0,&memory)||p_vkBindBufferMemory(device,buffer,memory,0))FAIL("buffer memory\n"); }
    { VkCommandPoolCreateInfo pi={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,0,0,family}; VkCommandBufferAllocateInfo ai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,VK_NULL_HANDLE,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1}; VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,0,0,0}; VkQueryPoolCreateInfo qi={VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,0,0,VK_QUERY_TYPE_TIMESTAMP,2,0}; if(p_vkCreateCommandPool(device,&pi,0,&pool)!=VK_SUCCESS||p_vkCreateQueryPool(device,&qi,0,&queries)!=VK_SUCCESS)FAIL("command/query create\n"); ai.commandPool=pool; if(p_vkAllocateCommandBuffers(device,&ai,&cmd)!=VK_SUCCESS)FAIL("command allocation\n"); if(p_vkBeginCommandBuffer(cmd,&bi)!=VK_SUCCESS)FAIL("command begin\n"); p_vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0); p_vkCmdFillBuffer(cmd,buffer,0,4096,0xAABBCCDDu); p_vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1); if(p_vkEndCommandBuffer(cmd)!=VK_SUCCESS)FAIL("command end\n"); }
    { VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd,0,0}; VkResult r=p_vkQueueSubmit(queue,1,&si,f1); if(r!=VK_SUCCESS)FAIL("first submit %s\n",name_result(r)); r=p_vkGetFenceStatus(device,f1); LOG("FENCE_POST_SUBMIT_STATUS: %s\n",name_result(r)); if(!fb&&r!=VK_SUCCESS&&r!=VK_NOT_READY)FAIL("bad post-submit status\n"); r=p_vkWaitForFences(device,1,&f1,VK_TRUE,0); LOG("FENCE_ZERO_WAIT: %s\n",name_result(r)); if(r!=VK_SUCCESS&&r!=VK_TIMEOUT)FAIL("zero wait bad result\n"); r=p_vkWaitForFences(device,1,&f1,VK_TRUE,UINT64_C(3000000000)); if(r!=VK_SUCCESS)FAIL("finite fence wait %s\n",name_result(r)); }
    { VkFence pair[2]={f1,f2}; VkResult r=p_vkWaitForFences(device,2,pair,VK_TRUE,0); LOG("FENCE_WAIT_ALL_ONE_PENDING: %s\n",name_result(r)); if(!fb&&r!=VK_TIMEOUT)FAIL("waitAll must timeout with one pending\n"); r=p_vkWaitForFences(device,2,pair,VK_FALSE,UINT64_MAX); if(r!=VK_SUCCESS)FAIL("waitAny %s\n",name_result(r)); if(p_vkResetFences(device,1,&f1)!=VK_SUCCESS)FAIL("reset fence\n"); if(!fb&&p_vkGetFenceStatus(device,f1)!=VK_NOT_READY)FAIL("reset status\n"); }
    { uint64_t res[4]={0,0,0,0}; VkResult r=p_vkGetQueryPoolResults(device,queries,0,2,sizeof(res),res,sizeof(uint64_t)*2,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT); LOG("QUERY_NOWAIT: %s q0=%llu avail0=%llu q1=%llu avail1=%llu\n",name_result(r),(unsigned long long)res[0],(unsigned long long)res[1],(unsigned long long)res[2],(unsigned long long)res[3]); if(r!=VK_SUCCESS&&r!=VK_NOT_READY)FAIL("query nonwait\n"); memset(res,0,sizeof(res)); r=p_vkGetQueryPoolResults(device,queries,0,2,sizeof(res),res,sizeof(uint64_t)*2,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT|VK_QUERY_RESULT_WAIT_BIT); LOG("QUERY_WAIT: %s t0=%llu t1=%llu avail=%llu/%llu\n",name_result(r),(unsigned long long)res[0],(unsigned long long)res[2],(unsigned long long)res[1],(unsigned long long)res[3]); if(r!=VK_SUCCESS||res[1]!=1||res[3]!=1||res[2]<res[0])FAIL("query waited values\n"); }
    for(i=0;i<125;i++) { VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd,0,0}; VkResult r; if(p_vkResetFences(device,1,&f1)!=VK_SUCCESS)FAIL("reuse reset %d\n",i); r=p_vkQueueSubmit(queue,1,&si,f1); if(r!=VK_SUCCESS)FAIL("reuse submit %d\n",i); r=p_vkWaitForFences(device,1,&f1,VK_TRUE,UINT64_MAX); if(r!=VK_SUCCESS)FAIL("reuse wait %d=%s\n",i,name_result(r)); }
    LOG("FENCE_REUSE: 125 submits/waits passed\n");
    { VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd,0,0}; if(p_vkQueueSubmit(queue,1,&si,f2)!=VK_SUCCESS)FAIL("pending destroy submit\n"); p_vkDestroyFence(device,f2,0); f2=VK_NULL_HANDLE; if(p_vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)!=VK_SUCCESS)FAIL("unfenced submit\n"); if(p_vkQueueWaitIdle(queue)!=VK_SUCCESS||p_vkDeviceWaitIdle(device)!=VK_SUCCESS)FAIL("idle after unfenced submit\n"); }
    { uint32_t *p=0; if(p_vkMapMemory(device,memory,0,4096,0,(void**)&p)!=VK_SUCCESS)FAIL("readback map\n"); mapped=1; if(p[0]!=0xAABBCCDDu||p[1023]!=0xAABBCCDDu)FAIL("readback fill mismatch %08X %08X\n",p[0],p[1023]); p_vkUnmapMemory(device,memory);mapped=0; LOG("READBACK: fill pattern verified\n"); }
cleanup:
    if(mapped&&memory) p_vkUnmapMemory(device,memory); if(device&&p_vkDeviceWaitIdle)p_vkDeviceWaitIdle(device); if(queries&&p_vkDestroyQueryPool)p_vkDestroyQueryPool(device,queries,0); if(pool&&p_vkDestroyCommandPool)p_vkDestroyCommandPool(device,pool,0); if(f1&&p_vkDestroyFence)p_vkDestroyFence(device,f1,0); if(f2&&p_vkDestroyFence)p_vkDestroyFence(device,f2,0); if(fs&&p_vkDestroyFence)p_vkDestroyFence(device,fs,0); if(buffer&&p_vkDestroyBuffer)p_vkDestroyBuffer(device,buffer,0); if(memory&&p_vkFreeMemory)p_vkFreeMemory(device,memory,0); if(device&&p_vkDestroyDevice)p_vkDestroyDevice(device,0); free(list); if(instance&&p_vkDestroyInstance)p_vkDestroyInstance(instance,0); if(mod)FreeLibrary(mod); if(failed){fprintf(stderr,"FINAL: FAIL\n");return 1;} LOG("FINAL: PASS FenceQueryProbe\n"); return 0;
}
