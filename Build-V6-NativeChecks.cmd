@echo off
setlocal
cd /d "%~dp0"
where cl.exe >nul 2>nul
if errorlevel 1 (
  echo Open an x64 Visual Studio Developer Command Prompt first.
  exit /b 1
)
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
  echo This command requires the x64 Visual Studio Developer Command Prompt.
  exit /b 1
)
cl.exe /nologo /std:c++17 /EHsc /utf-8 /O2 /MT "src\native-bc-copy-probe.cpp" /Fe"tests\NativeBcCopyProbe.exe" /Fo"tests\native-bc-copy-probe.obj" /link d3d11.lib dxgi.lib
if errorlevel 1 exit /b 1
if exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
  cl.exe /nologo /std:c++17 /EHsc /utf-8 /O2 /MT /I"%VULKAN_SDK%\Include" /I"src" "src\bc-copy-selector-test.cpp" /Fe"tests\BcCopySelectorTest.exe" /Fo"tests\bc-copy-selector-test.obj"
  if errorlevel 1 exit /b 1
) else (
  echo Selector test not built: optional official Vulkan SDK headers were not found.
)
echo Build complete. This script is optional and not tested in native Windows here.
exit /b 0
