@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>&1
if errorlevel 1 (
  echo Optional rebuild only: open an x64 Native Tools Command Prompt for Visual Studio first.
  exit /b 1
)
if not defined VULKAN_SDK (
  echo Set VULKAN_SDK to an official Vulkan SDK containing Include\vulkan\vulkan.h.
  exit /b 1
)
cl /nologo /O2 /W3 /std:c11 /I"%VULKAN_SDK%\Include" "src\texture_matrix.c" /Fe:"tests\TextureMatrix.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /std:c11 /I"%VULKAN_SDK%\Include" "src\bc_mode_check.c" /Fe:"tests\BCModeCheck.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
echo Optional diagnostics rebuilt. This command does not launch the game or alter drivers.
exit /b 0
