@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo Play RimWorld through the bridge WITH Vulkan recording (GFXReconstruct). The recording is large: keep the session short (about 2-3 minutes).
echo Close the game normally when done.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -QuickStart -Capture %*
set "EXITCODE=%ERRORLEVEL%"
if not exist "%~dp0reports" mkdir "%~dp0reports"
>"%~dp0reports\PLAY-DONE.txt" echo exit=%EXITCODE% capture=1
exit /b %EXITCODE%
