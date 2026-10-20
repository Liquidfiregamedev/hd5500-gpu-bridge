@echo off
setlocal
cd /d "%~dp0"
where cl.exe >nul 2>&1
if errorlevel 1 (
  echo Open a Visual Studio x64 Developer Command Prompt and run this command there.
  exit /b 1
)
if not defined VULKAN_SDK (
  echo Set VULKAN_SDK to your official Vulkan SDK directory, containing Include\vulkan\vulkan.h.
  exit /b 1
)
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
  echo Vulkan headers were not found under VULKAN_SDK\Include.
  exit /b 1
)
cl.exe /nologo /O2 /MT /std:c11 /W3 /I"%VULKAN_SDK%\Include" src\texture_upload_probe.c /Fe:tests\TextureUploadProbe.exe /Fo:tests\texture_upload_probe.obj /link /SUBSYSTEM:CONSOLE
exit /b %ERRORLEVEL%
