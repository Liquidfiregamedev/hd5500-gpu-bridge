@echo off
setlocal
cd /d "%~dp0"
set "PSEXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PSEXE=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if "%~1"=="" (
  "%PSEXE%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-WhiteScreenInfo.ps1" -BridgeFolder "%~dp0."
) else (
  "%PSEXE%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-WhiteScreenInfo.ps1" %*
)
set "RC=%ERRORLEVEL%"
echo.
if "%RW_BRIDGE_AUTOCLOSE%"=="1" (echo Finished. This window closes in 8 seconds. & timeout /t 8) else pause
exit /b %RC%
