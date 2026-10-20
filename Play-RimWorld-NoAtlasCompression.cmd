@echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo off
setlocal
cd /d "%~dp0"
echo Play RimWorld through the HD5500 bridge with GPU atlas compression OFF (-noCompressAtlases).
echo Test for black plant squares: if plants look right here, the engine's GPU BC7 encoder is the culprit.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -QuickStart -SwapchainImages 1 -GameArguments "-noCompressAtlases" %*
set "EXITCODE=%ERRORLEVEL%"
if not exist "%~dp0reports" mkdir "%~dp0reports"
>"%~dp0reports\PLAY-DONE.txt" echo exit=%EXITCODE% images=1 noCompressAtlases=1
exit /b %EXITCODE%
