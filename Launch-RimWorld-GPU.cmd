@echo off
setlocal
cd /d "%~dp0"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" %*
set "RESULT=%ERRORLEVEL%"
echo.
echo Launcher finished with code %RESULT%. Reports are in the reports folder.
echo Use your original Lavapipe launcher to return to the previous working setup.
if "%RW_BRIDGE_AUTOCLOSE%"=="1" (echo Finished. This window closes in 8 seconds. & timeout /t 8) else pause
exit /b %RESULT%
