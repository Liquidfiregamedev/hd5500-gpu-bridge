@echo off
setlocal
cd /d "%~dp0"
echo Optional source rebuild only. Precompiled diagnostics are already included.
echo Run from a Visual Studio x64 Native Tools Command Prompt.
where cl.exe >nul 2>&1
if errorlevel 1 (
 echo Microsoft x64 C compiler was not found. No changes made.
 exit /b 1
)
if not defined VULKAN_INCLUDE (
 echo Set VULKAN_INCLUDE to the directory containing the Vulkan headers folder.
 echo For example: set "VULKAN_INCLUDE=C:\VulkanSDK\your-version\Include"
 exit /b 1
)
cl.exe /nologo /std:c11 /O2 /W4 /MT /I"%VULKAN_INCLUDE%" "src\pooled_reuse_probe.c" /Fe:"tests\PooledReuseProbe.exe"
exit /b %ERRORLEVEL%
