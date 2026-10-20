@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo Play mode + dump every translated shader (HLSL) into reports\hlsl-dump for analysis.
if not exist "%~dp0reports\hlsl-dump" mkdir "%~dp0reports\hlsl-dump"
set "RW_GPU_DUMP_HLSL=%~dp0reports\hlsl-dump"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-HD5500-GPU.ps1" -QuickStart -SwapchainImages 1 -GameArguments "-noCompressAtlases" %*
exit /b %ERRORLEVEL%
