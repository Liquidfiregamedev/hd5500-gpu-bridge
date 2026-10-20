@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Check-Power.ps1"
if "%RW_BRIDGE_AUTOCLOSE%"=="1" (timeout /t 8) else pause
