@echo off
setlocal
cd /d "%~dp0"
echo Optional diagnostic source rebuild only; precompiled executables are included.
echo Use a Visual Studio x64 Native Tools Command Prompt.
where cl.exe >nul 2>&1
if errorlevel 1 (
 echo Microsoft x64 C compiler was not found. No changes made.
 exit /b 1
)
if not defined VULKAN_INCLUDE (
 echo Set VULKAN_INCLUDE to the directory containing the Vulkan headers folder.
 echo Example: set "VULKAN_INCLUDE=C:\VulkanSDK\your-version\Include"
 exit /b 1
)
cl.exe /nologo /std:c11 /O2 /W4 /MT /I"%VULKAN_INCLUDE%" "src\compact_parent_bind_probe.c" /Fe:"tests\CompactBufferProbe.exe"
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c11 /O2 /W4 /MT /I"%VULKAN_INCLUDE%" "src\vulkan10_gpu_icd_probe_suballocated.c" /Fe:"tests\SuballocatedComputeProbe.exe" user32.lib
exit /b %ERRORLEVEL%
