@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo UNATTENDED map test: Dev quicktest map, atlas compression OFF, frame dumps + shader dumps.
echo The game opens a test map by itself and closes after about 5 minutes. Do not click in it.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -QuickStart -SwapchainImages 1 -FrameDiag -DumpShaders -AutoCloseSeconds 300 -GameArguments "-quicktest -noCompressAtlases" %*
set "EXITCODE=%ERRORLEVEL%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Summarize-Run.ps1" -Label "map, no atlas compression"
exit /b %EXITCODE%
