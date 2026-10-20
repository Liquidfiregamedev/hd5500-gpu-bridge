@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo v13.4 frame diagnostics with 3 swapchain images (A/B test)
echo Run normally, NOT as Administrator.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -FrameProbe -UiTrace -FrameDiag -SwapchainImages 3 %*
set "EXITCODE=%ERRORLEVEL%"
echo.
echo Exit code: %EXITCODE%
echo Reports are in the extracted package reports folder.
if "%RW_BRIDGE_AUTOCLOSE%"=="1" (echo Finished. This window closes in 8 seconds. & timeout /t 8) else pause
exit /b %EXITCODE%
