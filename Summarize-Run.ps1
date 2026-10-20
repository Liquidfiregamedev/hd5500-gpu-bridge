# v13.45: adds a digest of the game's structured 1.7 Player.log (GPU selection, loader stage,
#   texture residency, VRAM offload, warnings by category) and a performance digest of [RW-GPU-PROF].
# v13.16: turn a gpu-run-* folder into one readable SUMMARY.txt.
# Usage: Summarize-Run.ps1 [-RunDir <path>] [-Label <text>]   (default: newest run in reports\)
param(
    [string]$RunDir = '',
    [string]$Label = ''
)
$ErrorActionPreference = 'Stop'
$reports = Join-Path $PSScriptRoot 'reports'
if (-not $RunDir) {
    $RunDir = (Get-ChildItem -LiteralPath $reports -Directory -Filter 'gpu-run-*' | Sort-Object Name | Select-Object -Last 1).FullName
}
if (-not $RunDir -or -not (Test-Path -LiteralPath $RunDir)) { throw 'No run folder found.' }

$stderrPath = Join-Path $RunDir 'game.stderr.txt'
$stdoutPath = Join-Path $RunDir 'game.stdout.txt'
$err = if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath } else { @() }
$out = if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath } else { @() }
$lines = New-Object System.Collections.Generic.List[string]
function Add([string]$text) { $lines.Add($text) }

$version = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'VERSION.txt') -ErrorAction SilentlyContinue | Select-Object -First 1
Add ('HD5500 GPU Bridge run summary  ' + (Split-Path $RunDir -Leaf) + $(if ($Label) { '  [' + $Label + ']' } else { '' }))
Add ('Bridge: ' + $version)
$args1 = $out | Where-Object { $_ -match 'Command line arguments:' } | Select-Object -First 1
if ($args1) { Add ('Game ' + ($args1 -replace '^.*?(Command line arguments:)', '$1')) }

# Timeline from the game's own log timestamps (HH:mm:ss at line start).
$stamps = @($out | Where-Object { $_ -match '^\d\d:\d\d:\d\d' } | ForEach-Object { $_.Substring(0, 8) })
if ($stamps.Count -gt 1) {
    $t0 = [TimeSpan]::Parse($stamps[0]); $t1 = [TimeSpan]::Parse($stamps[-1])
    Add ('Game log span: ' + $stamps[0] + ' -> ' + $stamps[-1] + ' (' + [int]($t1 - $t0).TotalSeconds + ' s)')
}
foreach ($marker in @('Effective Vulkan version', 'Selected GPU', 'GPU probe', 'GpuProbe', 'fallback stage', 'shaders will be compiled')) {
    $hit = $out | Where-Object { $_ -match [regex]::Escape($marker) } | Select-Object -First 1
    if ($hit) { Add ('  ' + $hit.Trim()) }
}

# v13.65: GPU compatibility block written by the renderer at device creation.
$compat = @($err | Where-Object { $_ -match '^\[RW-GPU-COMPAT\]' } | Select-Object -First 8)
if ($compat.Count) { Add '  GPU compatibility:'; $compat | ForEach-Object { Add ('    ' + ($_ -replace '^\[RW-GPU-COMPAT\]\s*', '')) } }

Add ''
Add '== Bridge events (count x line, first 120 chars) =='
$tags = '\[RW-GPU-(BLIT|DRAWBLIT|BLEND|SWIZZLE|CLEAR|WATCHDOG|DEVICE-FAIL|PROF|SHADERCACHE)\]|\[RW-D3D11-DEBUG\]|sampled-rt'
$events = $err | Where-Object { $_ -match $tags } | ForEach-Object { if ($_.Length -gt 120) { $_.Substring(0, 120) } else { $_ } }
if ($events) { $events | Group-Object | Sort-Object Count -Descending | Select-Object -First 25 | ForEach-Object { Add (('{0,5} x ' -f $_.Count) + $_.Name) } }
else { Add '  none' }
# RW-GPU-CAPS format trials probe unsupported formats on purpose; their HRESULTs are not problems.
$fatal = @($err | Where-Object { $_ -notmatch 'RW-GPU-CAPS|RW-GPU-SHADERCACHE' -and $_ -match 'WATCHDOG|DEVICE-FAIL|compilation failed|E_INVALIDARG|callHr=0x8|rejected' })
Add ('Serious bridge problems: ' + $fatal.Count + $(if ($fatal.Count) { '  <-- LOOK HERE' } else { '' }))

Add ''
Add '== Game errors/warnings (deduplicated, mod-metadata noise removed) =='
$noise = 'XML error|packageId|dependency|different vulkan-1.dll|Loader Message|Persistent allocation|modDependencies|empty display name'
$gameIssues = $out | Where-Object { $_ -match ' (ERROR|WARN) ' -and $_ -notmatch $noise } | ForEach-Object { ($_ -replace '^\d\d:\d\d:\d\d\s+', '').Trim() }
if ($gameIssues) { $gameIssues | Group-Object | Sort-Object Count -Descending | Select-Object -First 20 | ForEach-Object { $t = $_.Name; if ($t.Length -gt 160) { $t = $t.Substring(0, 160) }; Add (('{0,5} x ' -f $_.Count) + $t) } }
else { Add '  none' }
$stack = $out | Where-Object { $_ -match 'Exception' } | Select-Object -First 5
if ($stack) { Add '  Exceptions:'; $stack | ForEach-Object { Add ('    ' + $_.Trim()) } }

Add ''
Add '== Game log digest (Player.log, 1.7 structured format) =='
$playerPath = Join-Path $RunDir 'Player.log'
if (Test-Path -LiteralPath $playerPath) {
    $pl = Get-Content -LiteralPath $playerPath | Where-Object { $_ -notmatch '^\s+at ' }
    $ver = $pl | Where-Object { $_ -match '^Version:' } | Select-Object -First 1
    if ($ver) { Add ('  Game ' + $ver.Trim()) }
    foreach ($pat in @('\[GraphicsDevice\] (GPU \[|Selected GPU|Effective Vulkan|VK_EXT_memory_budget)', '\[VulkanStartup\]', '\[TextureResidency\]', 'over budget|offloaded|Evicted|refused a')) {
        $hits = @($pl | Where-Object { $_ -match $pat })
        $hits | Select-Object -First 4 | ForEach-Object { $t = ($_ -replace '^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+\s+', '').Trim(); if ($t.Length -gt 170) { $t = $t.Substring(0, 170) }; Add ('  ' + $t) }
        if ($hits.Count -gt 4) { Add ('  ... ' + ($hits.Count - 4) + ' more like this') }
    }
    $cats = $pl | Where-Object { $_ -match '^\S+ \S+\s+(WARN|ERROR) \[([^\]]+)\]' } | ForEach-Object { $Matches[1] + ' [' + $Matches[2] + ']' }
    if ($cats) { Add '  Warnings/errors by category:'; $cats | Group-Object | Sort-Object Count -Descending | Select-Object -First 10 | ForEach-Object { Add (('    {0,5} x ' -f $_.Count) + $_.Name) } }
} else { Add '  no Player.log in this run folder' }

Add ''
Add '== Bridge performance digest =='
$prof = @($err | Where-Object { $_ -match '\[RW-GPU-PROF\] slow command buffer (\d+) ms' } | ForEach-Object { [int]([regex]::Match($_, 'slow command buffer (\d+) ms').Groups[1].Value) })
if ($prof.Count) {
    $sorted = @($prof | Sort-Object)
    $p50 = $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.5)]; $p95 = $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.95)]
    Add ('  slow command buffers (>100 ms): ' + $prof.Count + '  median ' + $p50 + ' ms  p95 ' + $p95 + ' ms  max ' + $sorted[-1] + ' ms')
    $cmdCost = @{}
    foreach ($l in ($err | Where-Object { $_ -match '\[RW-GPU-PROF\] slow command buffer' })) {
        foreach ($m in [regex]::Matches($l, '(\w+Command) x\d+=(\d+)ms')) { $k = $m.Groups[1].Value; $cmdCost[$k] = [int]$cmdCost[$k] + [int]$m.Groups[2].Value }
    }
    $cmdCost.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5 | ForEach-Object { Add (('    {0,-28} {1,7} ms total in slow buffers' -f $_.Key, $_.Value)) }
} else { Add '  no slow command buffers logged' }
$sc = $err | Where-Object { $_ -match '\[RW-GPU-STATECACHE\]' } | Select-Object -Last 1
if ($sc) { Add ('  ' + $sc.Trim()) }
# v13.47: UAV slots rebuilt before a draw because the bound sets put a buffer in the wrong slot
# v13.48: frame pacing (freezes). FRAMES = 30 s summaries, HITCH = single frames over 250 ms.
$fr = @($err | Where-Object { $_ -match '\[RW-GPU-FRAMES\]' })
if ($fr.Count) { Add ('  Frame pacing (last 30 s window): ' + ($fr[-1] -replace '^.*\[RW-GPU-FRAMES\]\s*','')) } else { Add '  Frame pacing: no 30 s window completed' }
# v13.53/v13.55: frame-time statistics. "gameplay" leaves out start-up, loading pauses over 2 s and frames in
# which diagnostic pictures were saved (map tests save one every 20 s, ~1.3 s each); "session" is everything.
$fs = @($err | Where-Object { $_ -match '\[RW-GPU-FRAMESTATS\] session' })
$fg = @($err | Where-Object { $_ -match '\[RW-GPU-FRAMESTATS\] gameplay' })
$fx = @($err | Where-Object { $_ -match '\[RW-GPU-FRAMESTATS\] excluded' })
if ($fg.Count) {
    $line = $fg[-1] -replace '^.*\[RW-GPU-FRAMESTATS\]\s*gameplay\s*',''
    Add ('  Frame-time statistics (gameplay): ' + $line)
    if ($line -match 'avg=([\d.,]+)fps 1%low=([\d.,]+)fps 0\.1%low=([\d.,]+)fps') { Add ('    -> while playing: average ' + $Matches[1] + ' fps, 1% low ' + $Matches[2] + ' fps, 0.1% low ' + $Matches[3] + ' fps (the lows are what stutter feels like)') }
    if ($fx.Count) { Add ('    ' + ($fx[-1] -replace '^.*\[RW-GPU-FRAMESTATS\]\s*','')) }
}
if ($fs.Count) { Add ('  Frame-time statistics (whole session incl. loading): ' + ($fs[-1] -replace '^.*\[RW-GPU-FRAMESTATS\]\s*session\s*','')) }
if ($fs.Count -or $fg.Count) {
    $csv = Join-Path $RunDir 'frametimes.csv'
    if (Test-Path -LiteralPath $csv) { Add ('    per-frame data: frametimes.csv (open in Excel/LibreOffice; column frametime_ms; excluded: 0 gameplay, 1 diagnostic picture, 2 loading pause)') }
}
# v13.56: tick rate (read-only counter mod): achieved game ticks per second vs. the target (60 x speed).
$tk = @($err | Where-Object { $_ -match '\[RW-GPU-TICKS\] last' })
if ($tk.Count) {
    Add ('  Tick rate (game simulation speed): ' + ($tk[-1] -replace '^.*\[RW-GPU-TICKS\]\s*',''))
    if (Test-Path -LiteralPath (Join-Path $RunDir 'ticks.csv')) { Add '    per-second data: ticks.csv (ticks_per_s vs target_ticks_per_s)' }
}
# v13.57: what the frame spikes (gameplay frames over 50 ms) are made of.
#  bridge side: frametimes.csv, bridge_ms more than half of the frame = the bridge's own work
#  game side:   spikes.csv from the counter mod: GC pause, many simulation ticks, or other game work
$ftCsv = Join-Path $RunDir 'frametimes.csv'
if (Test-Path -LiteralPath $ftCsv) {
    try {
        $spk = @(Import-Csv -LiteralPath $ftCsv | Where-Object { $_.excluded -eq '0' -and [double]::Parse($_.frametime_ms, [Globalization.CultureInfo]::InvariantCulture) -gt 50 })
        if ($spk.Count) {
            $bridgeSide = @($spk | Where-Object { [double]::Parse($_.bridge_ms, [Globalization.CultureInfo]::InvariantCulture) -gt 0.5 * [double]::Parse($_.frametime_ms, [Globalization.CultureInfo]::InvariantCulture) }).Count
            Add ('  Frame spikes (gameplay frames over 50 ms): ' + $spk.Count + ' - ' + $bridgeSide + ' mostly bridge work, ' + ($spk.Count - $bridgeSide) + ' mostly game/.NET/Windows time')
        }
    } catch { }
}
$spCsv = Join-Path $RunDir 'spikes.csv'
if (Test-Path -LiteralPath $spCsv) {
    try {
        $rows = @(Import-Csv -LiteralPath $spCsv)
        if ($rows.Count) {
            $byCause = ($rows | Group-Object cause | Sort-Object Count -Descending | ForEach-Object { $_.Name + ' ' + $_.Count }) -join ', '
            Add ('    game-side view (counter mod, frames over 50 ms): ' + $byCause + '   (GC = .NET garbage collection pause; many-ticks = several simulation ticks in one frame at high speed; ticks / map-update / rest = largest part of the frame; other = older mod version)')
        }
    } catch { }
}
$gcl = @($err | Where-Object { $_ -match '\[RW-GPU-GC\]' })
if ($gcl.Count) {
    $g0 = 0; $g1 = 0; $g2 = 0; $gp = 0.0
    foreach ($l in $gcl) { if ($l -match 'gen0=(\d+) gen1=(\d+) gen2=(\d+), GC pause (\d+) ms') { $g0 += [int]$Matches[1]; $g1 += [int]$Matches[2]; $g2 += [int]$Matches[3]; $gp += [double]$Matches[4] } }
    Add ('  .NET garbage collection while in game: gen0 ' + $g0 + ', gen1 ' + $g1 + ', gen2 (full) ' + $g2 + ' collections, ' + [int]$gp + ' ms pause in total')
}
# v13.61: bridge cost per draw - the quicktest map differs per run (300-480 draws per frame), so runs are
# compared by bridge microseconds per draw rather than by fps alone.
$draws = @($err | Where-Object { $_ -match '\[RW-GPU-PROF\] sample command buffer' } | ForEach-Object { if ($_ -match 'DrawIndexedCommand x(\d+)=') { [int]$Matches[1] } } | Where-Object { $_ -ge 100 })
if ($draws.Count -and $fr.Count -and ($fr[-1] -match 'bridge=([\d.,]+)ms')) {
    $bridgeMs = [double]::Parse(($Matches[1] -replace ',', '.'), [Globalization.CultureInfo]::InvariantCulture)
    $sortedDraws = @($draws | Sort-Object); $medDraws = $sortedDraws[[int][Math]::Floor(($sortedDraws.Count - 1) / 2)]
    Add ('  Bridge cost per draw: {0:N1} microseconds  (bridge {1:N1} ms per frame / about {2} draws per frame; compare runs with this, the map differs per run)' -f (1000.0 * $bridgeMs / $medDraws), $bridgeMs, $medDraws)
}
# v13.59: frame phases from the counter mod (averages of all 10 s windows)
$ph = @($err | Where-Object { $_ -match '\[RW-GPU-PHASES\]' })
if ($ph.Count) {
    $pt = 0.0; $pm = 0.0; $pr = 0.0; $n = 0; $st = 0; $sm = 0; $sr = 0
    foreach ($l in $ph) {
        if ($l -match 'simulation ticks ([\d.]+) ms, world\+map update/drawing ([\d.]+) ms, rest \(UI, rendering, bridge, present\) ([\d.]+) ms \| frames over \d+ ms by largest part: ticks (\d+), map-update (\d+), rest (\d+)') {
            $inv = [Globalization.CultureInfo]::InvariantCulture
            $pt += [double]::Parse($Matches[1], $inv); $pm += [double]::Parse($Matches[2], $inv); $pr += [double]::Parse($Matches[3], $inv); $n++
            $st += [int]$Matches[4]; $sm += [int]$Matches[5]; $sr += [int]$Matches[6]
        }
    }
    if ($n) {
        Add ('  Where a frame goes (game side, average): simulation ticks {0:N1} ms, world+map update/drawing {1:N1} ms, rest (UI, rendering, bridge, present) {2:N1} ms' -f ($pt / $n), ($pm / $n), ($pr / $n))
        Add ('    frames over 50 ms by largest part: ticks ' + $st + ', map update/drawing ' + $sm + ', rest ' + $sr + '   (spikes.csv has ticks_ms / map_update_ms / rest_ms per slow frame)')
    }
}
$med = @($err | Where-Object { $_ -match '\[RW-GPU-PROF\] medium command buffer' })
if ($med.Count) {
    $mc = @{}
    foreach ($l in $med) { foreach ($m in [regex]::Matches($l, '(\w+Command) x\d+=(\d+)ms')) { $k = $m.Groups[1].Value; $mc[$k] = [int]$mc[$k] + [int]$m.Groups[2].Value } }
    Add ('  Bridge frames of 30-150 ms (sampled, ' + $med.Count + ' reports) - time by command:')
    $mc.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 4 | ForEach-Object { Add (('    {0,-28} {1,7} ms' -f $_.Key, $_.Value)) }
}
# v13.48: real CPU speed (Windows counter '% Processor Performance', sampled by the bridge 4x/s).
$speed = @($fr | ForEach-Object { if ($_ -match 'CPU speed avg=(\d+)% min=(\d+)% below-40%=(\d+) of (\d+)') { ,@([int]$Matches[1],[int]$Matches[2],[int]$Matches[3],[int]$Matches[4]) } })
if ($speed.Count) {
    $lowTotal = ($speed | ForEach-Object { $_[2] } | Measure-Object -Sum).Sum; $allTotal = ($speed | ForEach-Object { $_[3] } | Measure-Object -Sum).Sum
    $minAll = ($speed | ForEach-Object { $_[1] } | Measure-Object -Minimum).Minimum
    Add ('  CPU speed (100% = nominal clock): lowest ' + $minAll + '%, below 40% in ' + $lowTotal + ' of ' + $allTotal + ' samples')
    if ($lowTotal -gt 0) { Add '    The processor dropped to a fraction of its clock during the run (power/heat limit, e.g. Lenovo BD PROCHOT). Frames rendered then take 5-8x longer: most likely that stutter is the laptop throttling, not the bridge (note: idle cores also clock down; the speed probe in [RW-GPU-PROF] lines measures a busy core).' }
}
$hi = @($err | Where-Object { $_ -match '\[RW-GPU-HITCH\]' })
Add ('  Hitches over 250 ms logged: ' + $hi.Count + ' (frame dumps of a map test cause some of them)')
$hi | Select-Object -First 5 | ForEach-Object { Add ('    ' + ($_ -replace '^.*\[RW-GPU-HITCH\]\s*','')) }
$ur = @($err | Where-Object { $_ -match '\[RW-GPU-UAV-RESOLVE\]' })
if ($ur.Count) { Add ('  UAV slot corrections (forbidden-X fix, first 12 logged): ' + $ur.Count); $ur | Select-Object -First 3 | ForEach-Object { Add ('    ' + $_.Trim()) } } else { Add '  UAV slot corrections: none logged' }
$launcherLog = Join-Path $RunDir 'launcher.txt'
if (Test-Path -LiteralPath $launcherLog) { $nf = Get-Content -LiteralPath $launcherLog | Where-Object { $_ -match '^Number format:' } | Select-Object -First 1; if ($nf) { Add ('  ' + $nf.Trim()) } }

# v13.66: HD5500 Optimizer - RimWorld's own frame profiler (game-profile.txt) and bridge time by command.
Add ''
Add '== Game frame profile (HD5500 Optimizer, RimWorld frame profiler) =='
$gp = Join-Path $RunDir 'game-profile.txt'
if (Test-Path -LiteralPath $gp) {
    $gpl = @(Get-Content -LiteralPath $gp)
    $gpl | Select-Object -First 2 | ForEach-Object { Add ('  ' + $_) }
    $i = [Array]::IndexOf($gpl, ($gpl | Where-Object { $_ -like 'Whole session: scopes by SELF*' } | Select-Object -First 1))
    if ($i -ge 0) { $gpl[$i..([Math]::Min($i + 22, $gpl.Count - 1))] | ForEach-Object { Add ('  ' + $_) } }
    $j = [Array]::IndexOf($gpl, ($gpl | Where-Object { $_ -like 'Frames over 50 ms*' } | Select-Object -First 1))
    if ($j -ge 0) { $gpl[$j..([Math]::Min($j + 10, $gpl.Count - 1))] | ForEach-Object { Add ('  ' + $_) } }
} else { Add '  no game-profile.txt (profile off or the game ran under 30 s)' }
$ct = @($err | Where-Object { $_ -match '^\[RW-GPU-CMDTIME\]' })
if ($ct.Count) { Add '  Bridge time by command (last windows):'; $ct | Select-Object -Last 3 | ForEach-Object { Add ('    ' + ($_ -replace '^\[RW-GPU-CMDTIME\]\s*', '')) } }
$at = @($err | Where-Object { $_ -match '^\[RW-GPU-APITIME\]' })   # v13.68
if ($at.Count) { Add '  Frame-level Vulkan calls (render thread, last windows):'; $at | Select-Object -Last 3 | ForEach-Object { Add ('    ' + ($_ -replace '^\[RW-GPU-APITIME\]\s*', '')) } }
$ia = @($err | Where-Object { $_ -match '^\[RW-GPU-IA\]' } | Select-Object -Last 1)
if ($ia.Count) { Add ('  ' + ($ia[0] -replace '^\[RW-GPU-IA\]\s*', '')) }

Add ''
Add '== Frame diagnostics =='
$dumps = @($err | Where-Object { $_ -match 'RW_HD5500_FRAME_DIAG dump .*tag=swapchain' })
if ($dumps) { $dumps | ForEach-Object { Add ('  ' + ($_ -replace '^.*?dump ', '' -replace ' resFmt.*?decoded=1', '')) } }
else { Add '  no frame dumps (run with -FrameDiag)' }
$summaries = @($err | Where-Object { $_ -match 'frame-summary' })
if ($summaries) { Add ('  last frame-summary: ' + ($summaries[-1] -replace '^.*frame-summary', 'frame-summary')) }
$frames = Join-Path $RunDir 'frames'
if (Test-Path -LiteralPath $frames) { Add ('  BMP files: ' + @(Get-ChildItem -LiteralPath $frames -Filter '*.bmp').Count + ' in ' + $frames) }
$hlsl = Join-Path $RunDir 'hlsl'
if (Test-Path -LiteralPath $hlsl) { Add ('  HLSL dumps: ' + @(Get-ChildItem -LiteralPath $hlsl -Filter '*.hlsl').Count + ' in ' + $hlsl) }
$presents = @($err | Where-Object { $_ -match 'RW_HD5500_WSI present' }).Count
if ($presents) { Add ('  traced presents: ' + $presents) }

$summaryPath = Join-Path $RunDir 'SUMMARY.txt'
$lines | Set-Content -LiteralPath $summaryPath -Encoding UTF8
$lines | Set-Content -LiteralPath (Join-Path $reports 'LATEST-SUMMARY.txt') -Encoding UTF8
Write-Host ''
$lines | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ('Summary written to ' + $summaryPath)
