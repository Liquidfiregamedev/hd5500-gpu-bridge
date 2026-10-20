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
cl.exe /nologo /O2 /MT /std:c11 /W3 /I"%VULKAN_SDK%\Include" src\staging_buffer_bind_repro.c /Fe:tests\StagingBufferBindRepro.exe /Fo:tests\staging_buffer_bind_repro.obj /link /SUBSYSTEM:CONSOLE
exit /b %ERRORLEVEL%
