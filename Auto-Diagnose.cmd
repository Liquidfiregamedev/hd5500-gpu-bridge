@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo v13.4 UNATTENDED frame diagnostics (swapchain images: 1). The game closes itself after about 150 s.
del /q "%~dp0reports\AUTO-DONE.txt" 2>nul
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -FrameProbe -UiTrace -FrameDiag -SwapchainImages 1 -AutoCloseSeconds 150 %*
set "EXITCODE=%ERRORLEVEL%"
if not exist "%~dp0reports" mkdir "%~dp0reports"
>"%~dp0reports\AUTO-DONE.txt" echo exit=%EXITCODE% images=1
exit /b %EXITCODE%
