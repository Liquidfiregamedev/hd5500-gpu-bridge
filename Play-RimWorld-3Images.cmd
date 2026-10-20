@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo Play RimWorld through the HD5500 bridge (play mode: quick start, diagnostics off, swapchain images: 3).
echo Close the game normally when done. A report is still saved in the reports folder.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -QuickStart -SwapchainImages 3 %*
set "EXITCODE=%ERRORLEVEL%"
if not exist "%~dp0reports" mkdir "%~dp0reports"
>"%~dp0reports\PLAY-DONE.txt" echo exit=%EXITCODE% images=3
exit /b %EXITCODE%
