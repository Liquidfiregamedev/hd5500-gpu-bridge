@echo off
setlocal
cd /d "%~dp0"
echo WARNING (photosensitivity): the game window may flicker or flash during tests. Look away from unattended tests and stop if you feel unwell.
echo UNATTENDED full test: boot, map (compression off), map (compression on). About 10 minutes. Leave the PC alone.
call "%~dp0Auto-Test-Boot.cmd"
call "%~dp0Auto-Test-Map.cmd"
call "%~dp0Auto-Test-Map-Compressed.cmd"
powershell.exe -NoProfile -Command "Get-ChildItem %~dp0reports -Directory -Filter gpu-run-* | Sort-Object Name | Select-Object -Last 3 | ForEach-Object { Get-Content (Join-Path $_.FullName SUMMARY.txt) -ErrorAction SilentlyContinue;  } | Set-Content %~dp0reports\AUTO-TEST-ALL.txt"
echo Done. Combined report: reports\AUTO-TEST-ALL.txt
>"%~dp0reports\AUTO-DONE.txt" echo all-tests-done
