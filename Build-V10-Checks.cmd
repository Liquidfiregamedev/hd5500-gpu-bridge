@echo off
setlocal
cd /d "%~dp0"
where cl.exe >nul 2>&1
if errorlevel 1 (
  echo Optional rebuild only: open an x64 Visual Studio Native Tools Command Prompt.
  echo You do not need a compiler to use Check-GPU-Bridge.cmd or the game launcher.
  exit /b 1
)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
  echo Use an x64 Native Tools prompt. This package is Windows x64 only.
  exit /b 1
)
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
  echo Optional rebuild requires Vulkan SDK headers via VULKAN_SDK.
  echo Do not install development tools just to run the prebuilt package.
  exit /b 1
)
cl.exe /nologo /std:c11 /O2 /MT /I"%VULKAN_SDK%\Include" src\atlas_blit_probe.c /Fe:tests\AtlasBlitProbe.exe /Fo:tests\atlas_blit_probe.obj /link kernel32.lib
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c11 /O2 /MT /I"%VULKAN_SDK%\Include" src\failure_probe.c /Fe:tests\FailurePathProbe.exe /Fo:tests\failure_probe.obj /link kernel32.lib
if errorlevel 1 exit /b 1
echo Optional diagnostic rebuild completed. The Vulkan driver is not rebuilt by this script.
echo Run Check-GPU-Bridge.cmd before attempting the game.
exit /b 0
