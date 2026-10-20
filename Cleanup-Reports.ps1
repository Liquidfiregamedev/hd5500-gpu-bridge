# v13.56: free disk space used by the bridge's test results (C: on the test laptop is nearly full).
#
# What it does (only inside this package's reports\ folder, only names the bridge itself writes):
#   1. Keeps the newest -KeepRuns run folders (gpu-run-*) and their zips, the newest -KeepSelftests
#      self-test folders and the newest -KeepPowerChecks power checks; deletes older ones.
#   2. Converts the diagnostic pictures (*.bmp, 0.8-1.5 MB each) in the kept folders to PNG.
#      PNG is lossless (identical pixels) and 5-20x smaller for these pictures. The BMP is only
#      deleted after the PNG was written and could be read back with the same size.
#   3. Turns on Windows' built-in NTFS compression for the text logs (*.txt, *.err, *.log, *.csv,
#      *.hlsl) under reports\ (compact.exe). Transparent: the files open as usual, they just take
#      ~5-10x less space on disk. Undo any time with: compact /U /S:"<reports folder>"
#   -RemoveObsolete also deletes package folders that are no longer used (dozen-test: the Mesa
#   Dozen experiment, which cannot work on the HD 5500 - see RESEARCH-2026-10-19.md).
#   -WhatIf only reports what would happen.
# Writes a short report to reports\cleanup-<date-time>.txt and prints the space freed.
param(
    [int]$KeepRuns = 3,
    [int]$KeepSelftests = 1,
    [int]$KeepPowerChecks = 1,
    [switch]$NoCompress,
    [switch]$NoPng,
    [switch]$RemoveObsolete,
    [switch]$WhatIf,
    [switch]$Quiet
)
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$reports = Join-Path $root 'reports'
if (-not (Test-Path -LiteralPath $reports -PathType Container)) { if (-not $Quiet) { Write-Host 'No reports folder - nothing to clean.' }; return }
$log = New-Object System.Collections.Generic.List[string]
function Note([string]$text) { $log.Add($text); if (-not $Quiet) { Write-Host $text } }
function SizeOf([string]$path) {
    $s = (Get-ChildItem -LiteralPath $path -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    if ($s) { [long]$s } else { 0L }
}
function Remove-Old([string]$label, $items) {
    foreach ($item in $items) {
        $size = if ($item.PSIsContainer) { SizeOf $item.FullName } else { $item.Length }
        Note ('  delete {0,-40} {1,8:N1} MB  ({2})' -f $item.Name, ($size / 1MB), $label)
        if (-not $WhatIf) { Remove-Item -LiteralPath $item.FullName -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

$before = SizeOf $reports
$drive = (Get-Item -LiteralPath $root).PSDrive
$freeBefore = $drive.Free
Note ('Bridge cleanup ' + (Get-Date -Format 'yyyy-MM-dd HH:mm') + $(if ($WhatIf) { ' (WhatIf: nothing is changed)' } else { '' }))
Note ('reports\ before: {0:N0} MB; free on {1}: {2:N0} MB' -f ($before / 1MB), $drive.Name, ($freeBefore / 1MB))

# 1. old folders
$runs = @(Get-ChildItem -LiteralPath $reports -Directory | Where-Object { $_.Name -match '^gpu-run-\d{8}-\d{6}-\d{3}$' } | Sort-Object Name -Descending)
$keepRunNames = @($runs | Select-Object -First $KeepRuns | ForEach-Object { $_.Name })
Remove-Old ('older than the newest ' + $KeepRuns + ' runs') ($runs | Select-Object -Skip $KeepRuns)
$zips = @(Get-ChildItem -LiteralPath $reports -File -Filter 'gpu-run-*.zip' | Where-Object { $_.Name -match '^gpu-run-\d{8}-\d{6}-\d{3}\.zip$' -and $keepRunNames -notcontains $_.BaseName })
Remove-Old 'zip of a removed run' $zips
Remove-Old ('older than the newest ' + $KeepSelftests + ' self-test(s)') (Get-ChildItem -LiteralPath $reports -Directory | Where-Object { $_.Name -match '^selftest-\d{8}-\d{6}$' } | Sort-Object Name -Descending | Select-Object -Skip $KeepSelftests)
Remove-Old ('older than the newest ' + $KeepPowerChecks + ' power check(s)') (Get-ChildItem -LiteralPath $reports -Directory | Where-Object { $_.Name -match '^power-check-\d{8}-\d{6}$' } | Sort-Object Name -Descending | Select-Object -Skip $KeepPowerChecks)
Remove-Old 'older cleanup report' (Get-ChildItem -LiteralPath $reports -File -Filter 'cleanup-*.txt' | Sort-Object Name -Descending | Select-Object -Skip 2)
if ($RemoveObsolete) {
    foreach ($name in @('dozen-test')) {
        $p = Join-Path $root $name
        if (Test-Path -LiteralPath $p) { Remove-Old 'obsolete package folder' @(Get-Item -LiteralPath $p) }
    }
}

# 2. BMP -> PNG (lossless)
if (-not $NoPng) {
    Add-Type -AssemblyName System.Drawing
    $bmps = @(Get-ChildItem -LiteralPath $reports -Recurse -File -Filter '*.bmp' -ErrorAction SilentlyContinue)
    $saved = 0L; $converted = 0; $failed = 0
    foreach ($b in $bmps) {
        $png = [System.IO.Path]::ChangeExtension($b.FullName, '.png')
        if ($WhatIf) { $converted++; continue }
        try {
            $img = [System.Drawing.Image]::FromFile($b.FullName)
            try { $w = $img.Width; $h = $img.Height; $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $img.Dispose() }
            $check = [System.Drawing.Image]::FromFile($png)
            $ok = ($check.Width -eq $w -and $check.Height -eq $h); $check.Dispose()
            if ($ok) { $saved += $b.Length - (Get-Item -LiteralPath $png).Length; Remove-Item -LiteralPath $b.FullName -Force; $converted++ }
            else { Remove-Item -LiteralPath $png -Force -ErrorAction SilentlyContinue; $failed++ }
        } catch { $failed++ }
    }
    if ($bmps.Count) { Note ('  pictures: {0} BMP -> PNG{1}, {2:N0} MB saved{3}' -f $converted, $(if ($WhatIf) { ' (would convert)' } else { '' }), ($saved / 1MB), $(if ($failed) { ", $failed could not be converted (kept as BMP)" } else { '' })) }
}

# 3. NTFS compression of text logs (transparent)
if (-not $NoCompress -and -not $WhatIf) {
    $compact = Join-Path $env:SystemRoot 'System32\compact.exe'
    if (Test-Path -LiteralPath $compact) {
        foreach ($pattern in @('*.txt', '*.err', '*.log', '*.csv', '*.hlsl', '*.html')) {
            & $compact /C /S:"$reports" /I /Q $pattern | Out-Null
        }
        Note '  text logs: NTFS compression on (transparent; undo with compact /U /S:"<reports folder>")'
    }
}

$after = SizeOf $reports
$freeAfter = (Get-Item -LiteralPath $root).PSDrive.Free
Note ('reports\ after: {0:N0} MB (file size; compressed logs use less on disk). Free on {1}: {2:N0} MB (+{3:N0} MB)' -f ($after / 1MB), $drive.Name, ($freeAfter / 1MB), (($freeAfter - $freeBefore) / 1MB))
if (-not $WhatIf) {
    try { $log | Set-Content -LiteralPath (Join-Path $reports ('cleanup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')) -Encoding UTF8 } catch { }
}
