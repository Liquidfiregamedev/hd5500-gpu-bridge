@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo UNATTENDED map test with the engine default (GPU atlas compression ON), for A/B against Auto-Test-Map.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -QuickStart -SwapchainImages 1 -FrameDiag -AutoCloseSeconds 300 -GameArguments "-quicktest" %*
set "EXITCODE=%ERRORLEVEL%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Summarize-Run.ps1" -Label "map, atlas compression ON"
exit /b %EXITCODE%
