#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c,m) do{if(!(c)){printf("FAIL: %s\n",m);return 1;}printf("PASS: %s\n",m);}while(0)
int main(int argc,char **argv){
 if(argc<2)return 2;HMODULE dll=LoadLibraryExA(argv[1],NULL,LOAD_WITH_ALTERED_SEARCH_PATH);CHECK(dll,"proxy loads");
 PFN_vkCreateInstance create=(PFN_vkCreateInstance)GetProcAddress(dll,"vkCreateInstance");
 PFN_vkCreateDevice devicecreate=(PFN_vkCreateDevice)GetProcAddress(dll,"vkCreateDevice");
 PFN_vkGetInstanceProcAddr gipa=(PFN_vkGetInstanceProcAddr)GetProcAddress(dll,"vkGetInstanceProcAddr");
 CHECK(create&&devicecreate&&gipa,"wrapped entry points exported");
 const char *instance_ext[]={"VK_KHR_surface"};VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_0;
 VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;ci.enabledExtensionCount=1;ci.ppEnabledExtensionNames=instance_ext;
 VkInstance instance=VK_NULL_HANDLE;
 if(argc>2&&!strcmp(argv[2],"missing")){CHECK(create(&ci,NULL,&instance)==VK_ERROR_INITIALIZATION_FAILED,"missing backend fails honestly without crash");return 0;}
 SetEnvironmentVariableA("RW_HD5500_PORTABILITY",NULL);
 CHECK(create(&ci,NULL,&instance)==VK_ERROR_EXTENSION_NOT_PRESENT,"disabled opt-in preserves original extension error");
 SetEnvironmentVariableA("RW_HD5500_PORTABILITY","1");
 PFN_vkCreateInstance indirect=(PFN_vkCreateInstance)gipa(VK_NULL_HANDLE,"vkCreateInstance");CHECK(indirect,"GIPA exposes wrapped instance creation");
 CHECK(indirect(&ci,NULL,&instance)==VK_ERROR_FEATURE_NOT_PRESENT,"advertised portability is added and native feature error is retained");
 CHECK(ci.flags==0&&ci.enabledExtensionCount==1&&ci.ppEnabledExtensionNames==instance_ext&&app.apiVersion==VK_API_VERSION_1_0,"original instance flags, extension list and API version unchanged");
 CHECK(!gipa(VK_NULL_HANDLE,"vkCreateDevice")&&!gipa(VK_NULL_HANDLE,"vkImaginaryFunction"),"real loader null-function behavior preserved");
 const char *device_ext[]={"VK_KHR_swapchain"};VkPhysicalDeviceFeatures features={0};features.shaderFloat64=VK_TRUE;
 VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.enabledExtensionCount=1;dci.ppEnabledExtensionNames=device_ext;dci.pEnabledFeatures=&features;VkDevice device=VK_NULL_HANDLE;
 CHECK(devicecreate((VkPhysicalDevice)(uintptr_t)1,&dci,NULL,&device)==VK_ERROR_FEATURE_NOT_PRESENT,"advertised subset opt-in does not suppress unsupported-feature failure");
 CHECK(dci.enabledExtensionCount==1&&dci.ppEnabledExtensionNames==device_ext&&dci.pEnabledFeatures==&features&&features.shaderFloat64,"original device features and extension list unchanged");
 SetEnvironmentVariableA("RW_HD5500_PORTABILITY",NULL);
 CHECK(devicecreate((VkPhysicalDevice)(uintptr_t)1,&dci,NULL,&device)==VK_ERROR_EXTENSION_NOT_PRESENT,"disabled device opt-in preserves original request/error");
 puts("PASS: MOCK unit tests complete; no actual GPU or game was tested.");return 0;
}
