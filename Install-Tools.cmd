@echo off
setlocal
cd /d "%~dp0"
echo Installs the diagnostic tools (RenderDoc, PIX, Intel GPA, Windows Performance Toolkit, PresentMon, HWiNFO, Vulkan SDK, Dependencies, Process Monitor, dnSpyEx, dotnet-trace). Each one is optional; closes this window when done.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Tools.ps1" %*
if "%RW_BRIDGE_AUTOCLOSE%"=="1" (echo Finished. This window closes in 8 seconds. & timeout /t 8) else pause
