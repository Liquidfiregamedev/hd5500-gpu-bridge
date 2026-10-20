@echo off
rem v13.64: (re)creates the "HD5500 GPU Bridge" shortcut on the desktop for THIS copy of the
rem package (use it after moving the folder, e.g. to a USB drive). Only creates a .lnk file.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$s = (New-Object -ComObject WScript.Shell).CreateShortcut([IO.Path]::Combine([Environment]::GetFolderPath('Desktop'), 'HD5500 GPU Bridge.lnk')); $s.TargetPath = '%~dp0Bridge-Menu.cmd'; $s.WorkingDirectory = '%~dp0'; $s.Description = 'HD5500 GPU Bridge menu'; $s.Save(); Write-Host ('Shortcut created: ' + $s.FullName + ' -> ' + $s.TargetPath)"
pause
