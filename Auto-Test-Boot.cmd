@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo UNATTENDED boot test: start the game, reach the main menu, quit by itself (-QuitAfterBoot). Measures boot time.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -QuickStart -SwapchainImages 1 -AutoCloseSeconds 240 -GameArguments "-QuitAfterBoot" %*
set "EXITCODE=%ERRORLEVEL%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Summarize-Run.ps1" -Label "boot only"
exit /b %EXITCODE%
