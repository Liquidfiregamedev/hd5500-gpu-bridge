@echo off
setlocal
cd /d "%~dp0"
where cl.exe >nul 2>nul
if errorlevel 1 (
  echo Open an x64 Visual Studio Developer Command Prompt first.
  exit /b 1
)
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
  echo This build expects the x64 Visual Studio Developer Command Prompt.
  exit /b 1
)
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
  echo Install the official Vulkan SDK for headers and set VULKAN_SDK.
  echo Building is optional. The supplied executable checks are ready to run.
  exit /b 1
)
cl.exe /nologo /TC /std:c11 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /I"%VULKAN_SDK%\Include" "src\bc7_matrix_probe.c" /Fe"tests\BC7MatrixProbe.exe" /Fo"tests\bc7_matrix_probe.obj" /link /SUBSYSTEM:CONSOLE kernel32.lib
if errorlevel 1 exit /b 1
cl.exe /nologo /TC /std:c11 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /I"%VULKAN_SDK%\Include" "src\bc7_gpu_source_probe.c" /Fe"tests\BC7GpuSourceProbe.exe" /Fo"tests\bc7_gpu_source_probe.obj" /link /SUBSYSTEM:CONSOLE kernel32.lib
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++17 /EHsc /utf-8 /O2 /MT /I"src" "src\native-map-fixture.cpp" /Fe"tests\NativeResourceMapFixture.exe" /Fo"tests\native-map-fixture.obj" /link d3d11.lib dxgi.lib
if errorlevel 1 exit /b 1
echo Build complete. This script itself has not been tested in native Windows here.
exit /b 0
