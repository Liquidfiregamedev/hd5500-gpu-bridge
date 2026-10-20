@echo off
rem v13.56: free disk space used by bridge test results (see Cleanup-Reports.ps1)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Cleanup-Reports.ps1" %*
if not "%RW_BRIDGE_AUTOCLOSE%"=="1" pause
if "%RW_BRIDGE_AUTOCLOSE%"=="1" timeout /t 8
