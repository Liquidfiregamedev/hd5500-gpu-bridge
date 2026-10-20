/*
 * MIT-licensed compatibility proxy, 2026.
 * Forwards to a byte-identical copy of the user's installed Khronos loader.
 * With RW_HD5500_PORTABILITY=1 only, adds advertised portability opt-in.
 * Does not implement Vulkan, alter GPU properties, hide errors, or bypass ICDs.
 */
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HMODULE self_module, backend;
static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
static PFN_vkCreateInstance real_create_instance;
static PFN_vkCreateDevice real_create_device;
static PFN_vkEnumerateInstanceExtensionProperties real_instance_extensions;
static PFN_vkEnumerateDeviceExtensionProperties real_device_extensions;

static void note(const char *operation,int result) {
    char directory[MAX_PATH],path[MAX_PATH];
    DWORD n=GetEnvironmentVariableA("LOCALAPPDATA",directory,MAX_PATH);
    if(!n||n>=MAX_PATH)return;
    if(snprintf(path,MAX_PATH,"%s\\RimWorldVulkanBridge",directory)<0)return;
    CreateDirectoryA(path,NULL);
    if(snprintf(path,MAX_PATH,"%s\\RimWorldVulkanBridge\\portability-proxy-%lu.log",directory,GetCurrentProcessId())<0)return;
    FILE *f=fopen(path,"ab");if(!f)return;
    fprintf(f,"%lu %s result=%d\n",GetTickCount(),operation,result);fclose(f);
}
static BOOL CALLBACK initialize(PINIT_ONCE state,PVOID parameter,PVOID *context) {
    (void)state;(void)parameter;(void)context;
    wchar_t path[32768];
    DWORD n=GetModuleFileNameW(self_module,path,32768);
    if(!n||n>=32768)return TRUE;
    wchar_t *slash=wcsrchr(path,L'\\');if(!slash)return TRUE;
    if((size_t)(slash-path)+24>=32768)return TRUE;
    wcscpy(slash+1,L"rw_system_vulkan.dll");
    backend=LoadLibraryExW(path,NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!backend){note("ERROR loading original-loader copy",(int)GetLastError());return TRUE;}
    real_gipa=(PFN_vkGetInstanceProcAddr)GetProcAddress(backend,"vkGetInstanceProcAddr");
    real_gdpa=(PFN_vkGetDeviceProcAddr)GetProcAddress(backend,"vkGetDeviceProcAddr");
    real_create_instance=(PFN_vkCreateInstance)GetProcAddress(backend,"vkCreateInstance");
    real_create_device=(PFN_vkCreateDevice)GetProcAddress(backend,"vkCreateDevice");
    real_instance_extensions=(PFN_vkEnumerateInstanceExtensionProperties)GetProcAddress(backend,"vkEnumerateInstanceExtensionProperties");
    real_device_extensions=(PFN_vkEnumerateDeviceExtensionProperties)GetProcAddress(backend,"vkEnumerateDeviceExtensionProperties");
    note("Original Khronos loader initialized",backend?0:-1);
    return TRUE;
}
static int ready(void){InitOnceExecuteOnce(&once,initialize,NULL,NULL);return backend&&real_gipa&&real_gdpa&&real_create_instance&&real_create_device;}
static int enabled(void){char value[8];return GetEnvironmentVariableA("RW_HD5500_PORTABILITY",value,sizeof(value))==1&&value[0]=='1';}
static int listed(uint32_t count,const char *const *names,const char *wanted){for(uint32_t i=0;i<count;i++)if(names[i]&&!strcmp(names[i],wanted))return 1;return 0;}
static int advertised(VkPhysicalDevice physical,const char *wanted) {
    uint32_t count=0;VkResult result;
    if(physical){if(!real_device_extensions)return 0;result=real_device_extensions(physical,NULL,&count,NULL);}
    else {if(!real_instance_extensions)return 0;result=real_instance_extensions(NULL,&count,NULL);}
    if(result!=VK_SUCCESS||!count||count>65536)return 0;
    VkExtensionProperties *properties=calloc(count,sizeof(*properties));if(!properties)return 0;
    if(physical)result=real_device_extensions(physical,NULL,&count,properties);
    else result=real_instance_extensions(NULL,&count,properties);
    int found=0;if(result==VK_SUCCESS)for(uint32_t i=0;i<count;i++)if(!strcmp(properties[i].extensionName,wanted)){found=1;break;}
    free(properties);return found;
}
__declspec(dllexport) VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *allocator,VkInstance *instance) {
    if(!ready())return VK_ERROR_INITIALIZATION_FAILED;
    if(!info||!enabled()||!advertised(VK_NULL_HANDLE,VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))return real_create_instance(info,allocator,instance);
    VkInstanceCreateInfo adjusted=*info;
    const char **extensions=NULL;
    if(!listed(info->enabledExtensionCount,info->ppEnabledExtensionNames,VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        if(info->enabledExtensionCount>65536)return VK_ERROR_INITIALIZATION_FAILED;
        extensions=malloc(((size_t)info->enabledExtensionCount+1)*sizeof(*extensions));if(!extensions)return VK_ERROR_OUT_OF_HOST_MEMORY;
        if(info->enabledExtensionCount)memcpy(extensions,info->ppEnabledExtensionNames,info->enabledExtensionCount*sizeof(*extensions));
        extensions[info->enabledExtensionCount]=VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        adjusted.ppEnabledExtensionNames=extensions;adjusted.enabledExtensionCount++;
    }
    adjusted.flags|=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    VkResult result=real_create_instance(&adjusted,allocator,instance);
    note("Instance portability opt-in; original API/features retained",result);
    free(extensions);return result;
}
__declspec(dllexport) VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,const VkAllocationCallbacks *allocator,VkDevice *device) {
    if(!ready())return VK_ERROR_INITIALIZATION_FAILED;
    const char *subset="VK_KHR_portability_subset";
    if(!info||!enabled()||listed(info->enabledExtensionCount,info->ppEnabledExtensionNames,subset)||!advertised(physical,subset))return real_create_device(physical,info,allocator,device);
    if(info->enabledExtensionCount>65536)return VK_ERROR_INITIALIZATION_FAILED;
    const char **extensions=malloc(((size_t)info->enabledExtensionCount+1)*sizeof(*extensions));if(!extensions)return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(info->enabledExtensionCount)memcpy(extensions,info->ppEnabledExtensionNames,info->enabledExtensionCount*sizeof(*extensions));
    extensions[info->enabledExtensionCount]=subset;
    VkDeviceCreateInfo adjusted=*info;adjusted.enabledExtensionCount++;adjusted.ppEnabledExtensionNames=extensions;
    VkResult result=real_create_device(physical,&adjusted,allocator,device);
    note("Device portability-subset extension opt-in; feature requests unchanged",result);
    free(extensions);return result;
}
__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char *name);
__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name);
__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    if(!ready()||!name)return NULL;
    PFN_vkVoidFunction original=real_gipa(instance,name);
    if(!original)return NULL;
    if(!strcmp(name,"vkCreateInstance"))return(PFN_vkVoidFunction)vkCreateInstance;
    if(!strcmp(name,"vkCreateDevice"))return(PFN_vkVoidFunction)vkCreateDevice;
    if(!strcmp(name,"vkGetInstanceProcAddr"))return(PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if(!strcmp(name,"vkGetDeviceProcAddr"))return(PFN_vkVoidFunction)vkGetDeviceProcAddr;
    return original;
}
__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name) {
    if(!ready()||!name)return NULL;
    PFN_vkVoidFunction original=real_gdpa(device,name);
    if(!original)return NULL;
    if(!strcmp(name,"vkGetDeviceProcAddr"))return(PFN_vkVoidFunction)vkGetDeviceProcAddr;
    return original;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved){(void)reserved;if(reason==DLL_PROCESS_ATTACH){self_module=instance;DisableThreadLibraryCalls(instance);}return TRUE;}
