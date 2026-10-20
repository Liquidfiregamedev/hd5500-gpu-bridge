#requires -Version 5.1
<#
Read-only live RimWorld hang diagnostics. Does not start/stop processes, inject,
change settings, change drivers, register anything, or upload files.
Only creates a local report folder and ZIP. Captures two snapshots 15 seconds apart.
#>
[CmdletBinding()]
param([string]$BridgeFolder = '')
$ErrorActionPreference = 'Stop'
Write-Host 'White-screen collector revision 2: explicit validated paths.'
$scriptDirectory = [string]$PSScriptRoot
if ([string]::IsNullOrWhiteSpace($scriptDirectory)) {
    $scriptFile = [string]$MyInvocation.MyCommand.Path
    if (-not [string]::IsNullOrWhiteSpace($scriptFile)) { $scriptDirectory = [IO.Path]::GetDirectoryName($scriptFile) }
}
if ([string]::IsNullOrWhiteSpace($scriptDirectory)) { $scriptDirectory = (Get-Location).Path }
if ([string]::IsNullOrWhiteSpace($BridgeFolder)) { $BridgeFolder = $scriptDirectory }
if ([string]::IsNullOrWhiteSpace($BridgeFolder) -or -not (Test-Path -LiteralPath $BridgeFolder -PathType Container)) {
    throw 'Bridge folder is unavailable. Run the collector CMD from the extracted v3 folder.'
}
$BridgeFolder = (Resolve-Path -LiteralPath $BridgeFolder).Path
$desktop = [Environment]::GetFolderPath('DesktopDirectory')
if ([string]::IsNullOrWhiteSpace($desktop) -or -not (Test-Path -LiteralPath $desktop -PathType Container)) { $desktop = $BridgeFolder }
$out = Join-Path $desktop ('RimWorld-WhiteScreen-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $out | Out-Null
$notes = New-Object System.Collections.Generic.List[string]
$notes.Add('Read-only live collector. No process was started/stopped and no settings changed.')
$notes.Add('Thread wait reasons are not stack traces. CPU/log progress does not by itself prove a particular cause.')
$notes.Add('Source path information may appear in logs. Inspect the report before sharing it.')
$notes.Add('Bridge folder: ' + $BridgeFolder)
$profileDirectory = [string]$env:USERPROFILE
if ([string]::IsNullOrWhiteSpace($profileDirectory)) { $profileDirectory = [Environment]::GetFolderPath('UserProfile') }
if ([string]::IsNullOrWhiteSpace($profileDirectory)) { throw 'Windows user-profile directory is unavailable; collect Player.log manually.' }
$gameData = Join-Path $profileDirectory 'AppData\LocalLow\Ludeon Studios\RimWorld by Ludeon Studios'
$player = Join-Path $gameData 'Logs\Player.log'
$run = $null
$reportRoot = Join-Path $BridgeFolder 'reports'
if (Test-Path -LiteralPath $reportRoot -PathType Container) {
    $run = Get-ChildItem -LiteralPath $reportRoot -Directory -Filter 'gpu-run-*' | Sort-Object CreationTimeUtc -Descending | Select-Object -First 1
}
if (-not $run) { $notes.Add('No bridge reports folder found here; Player.log and process sampling still collected. Put this collector in the v3 folder to include bridge streams.') }

function Copy-LiveTail([string]$Source, [string]$Destination) {
    if ([string]::IsNullOrWhiteSpace($Source) -or [string]::IsNullOrWhiteSpace($Destination)) { $notes.Add('Skipped an empty log source/destination path.'); return }
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { $notes.Add('Not found: ' + $Source); return }
    $stream = $null
    try {
        $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream = [IO.File]::Open($Source,[IO.FileMode]::Open,[IO.FileAccess]::Read,$share)
        $length = $stream.Length
        $count = [int][Math]::Min([long]4194304,$length)
        $start = [long]($length - $count)
        [void]$stream.Seek($start,[IO.SeekOrigin]::Begin)
        $bytes = New-Object byte[] $count
        $read = 0
        while ($read -lt $count) {
            $n = $stream.Read($bytes,$read,$count - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        if ($read -lt $count) { [Array]::Resize([ref]$bytes,$read) }
        [IO.File]::WriteAllBytes($Destination,$bytes)
        $notes.Add(('Snapshot {0}: source={1}; originalBytes={2}; startByte={3}; copiedBytes={4}; utc={5}' -f $Destination,$Source,$length,$start,$read,[DateTime]::UtcNow.ToString('o')))
    } catch { $notes.Add('Could not snapshot ' + $Source + ': ' + $_.Exception.Message) }
    finally { if ($stream) { $stream.Dispose() } }
}

function Take-Snapshot([string]$Label) {
    $records = @()
    Copy-LiveTail $player (Join-Path $out ($Label + '-Player.log'))
    if ($run) {
        foreach ($name in @('game.stdout.txt','game.stderr.txt','launcher.txt')) {
            Copy-LiveTail (Join-Path $run.FullName $name) (Join-Path $out ($Label + '-' + $name))
        }
    }
    foreach ($p in @(Get-Process -Name RimWorld -ErrorAction SilentlyContinue)) {
        try {
            $p.Refresh()
            $record = [PSCustomObject]@{
                CapturedUtc = [DateTime]::UtcNow.ToString('o')
                Id = $p.Id
                StartTimeUtc = $p.StartTime.ToUniversalTime().ToString('o')
                CpuSeconds = $p.TotalProcessorTime.TotalSeconds
                Responding = $p.Responding
                WorkingSetBytes = $p.WorkingSet64
                PrivateBytes = $p.PrivateMemorySize64
                ThreadCount = $p.Threads.Count
            }
            $records += $record
            $threads = @()
            foreach ($t in $p.Threads) {
                try {
                    $reason = ''
                    if ($t.ThreadState -eq [Diagnostics.ThreadState]::Wait) { $reason = [string]$t.WaitReason }
                    $threads += [PSCustomObject]@{ Id=$t.Id; State=[string]$t.ThreadState; WaitReason=$reason; CpuSeconds=$t.TotalProcessorTime.TotalSeconds }
                } catch { $notes.Add('Thread query: ' + $_.Exception.Message) }
            }
            ConvertTo-Json -InputObject @($threads) -Depth 4 | Set-Content -LiteralPath (Join-Path $out ($Label + '-threads-' + $p.Id + '.json')) -Encoding UTF8
            if ($Label -eq 'first') {
                try {
                    $modules = @($p.Modules | Where-Object { $_.ModuleName -match 'vulkan|ashes|d3d11|dxgi|d3dcompiler|rw_system' } | Select-Object ModuleName,FileName)
                    ConvertTo-Json -InputObject $modules -Depth 4 | Set-Content -LiteralPath (Join-Path $out ('rendering-modules-' + $p.Id + '.json')) -Encoding UTF8
                } catch { $notes.Add('Module query: ' + $_.Exception.Message) }
            }
        } catch { $notes.Add('Process query: ' + $_.Exception.Message) }
    }
    ConvertTo-Json -InputObject @($records) -Depth 4 | Set-Content -LiteralPath (Join-Path $out ($Label + '-processes.json')) -Encoding UTF8
    return $records
}

Write-Host 'Leave the white-screen game and its original launcher OPEN. This collector only reads diagnostics.'
$first = @(Take-Snapshot 'first')
Write-Host 'Waiting 15 seconds to compare CPU and log progress...'
Start-Sleep -Seconds 15
$second = @(Take-Snapshot 'second')
$progress = @()
foreach ($after in $second) {
    $before = $first | Where-Object { $_.Id -eq $after.Id -and $_.StartTimeUtc -eq $after.StartTimeUtc } | Select-Object -First 1
    if ($before) {
        $elapsed = ([DateTime]::Parse($after.CapturedUtc) - [DateTime]::Parse($before.CapturedUtc)).TotalSeconds
        $delta = $after.CpuSeconds - $before.CpuSeconds
        $progress += [PSCustomObject]@{ Id=$after.Id; IntervalSeconds=$elapsed; CpuSecondsDelta=$delta; RespondingBefore=$before.Responding; RespondingAfter=$after.Responding }
    }
}
ConvertTo-Json -InputObject @($progress) -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'cpu-progress.json') -Encoding UTF8
if (-not $first.Count -and -not $second.Count) { $notes.Add('No process named RimWorld found. Run while the white screen is still open.') }
$notes | Set-Content -LiteralPath (Join-Path $out 'collection-notes.txt') -Encoding UTF8
$zip = $out + '.zip'
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip
Write-Host ('Created: ' + $zip)
Write-Host 'Inspect it, then attach this ZIP in the conversation. Nothing was uploaded automatically.'
