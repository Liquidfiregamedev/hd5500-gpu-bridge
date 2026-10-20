@echo off
setlocal
cd /d "%~dp0"
echo v13 optional full checks
echo Run normally, NOT as Administrator.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -TestOnly -FullChecks %*
set "EXITCODE=%ERRORLEVEL%"
echo.
echo Exit code: %EXITCODE%
echo Reports are in the extracted package reports folder.
if "%RW_BRIDGE_AUTOCLOSE%"=="1" (echo Finished. This window closes in 8 seconds. & timeout /t 8) else pause
exit /b %EXITCODE%
