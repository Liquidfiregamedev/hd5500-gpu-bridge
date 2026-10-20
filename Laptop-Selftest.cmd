@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo Bridge self-test on this PC: runs the developer test suite on the real GPU and on the WARP software reference.
echo Takes 1-2 minutes. RimWorld is not started. Result: reports\LATEST-SELFTEST.txt
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Laptop-Selftest.ps1" %*
if "%RW_BRIDGE_AUTOCLOSE%"=="1" (echo Finished. This window closes in 8 seconds. & timeout /t 8) else pause
