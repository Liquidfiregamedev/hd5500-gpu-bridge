# v13.53: uavalias (two storage-buffer descriptors into ONE buffer, like Waylith's instance transforms + colours:
#          D3D11 drops the whole UAV bind unless the bridge binds a private copy) and uavalias-old (RW_GPU_UAV_ALIAS_COPY=0;
#          on a real driver this is expected to FAIL = proof of the geyser-steam bug; Wine does not enforce the rule).
# v13.47b: uavoffset (storage-buffer descriptor with a non-zero offset into a shared ring buffer: the shader must
#          read from that offset; whole-buffer binding = the forbidden X never drawn) and uavoffset-old (RW_GPU_UAV_RANGE=0).
# v13.47: uavlayout (descriptor sets bound with a sibling pipeline layout: the instance buffer must land in the
#         UAV slot the drawing pipeline's shader reads; this is the forbidden-X slot mix-up) and uavlayout-old
#         (same with the fix off, RW_GPU_UAV_RESOLVE=0: must show the old bug, plain red).
# v13.44: instuav-old now expects the bug picture (proof run stays green when Windows keeps behaving as measured).
# v13.43: instuav (graphics UAV re-bound after the bridge's helper passes; cause of the flickering forbidden X).
# v13.42: water tolerance 4 samples over 12 (smooth noise texture); first laptop result: HD 5500 == WARP == lavapipe within 2.3 levels.
# v13.41: water (RimWorld's two-pass water with the game's own shaders, compared pixel-by-pixel
#   against Mesa lavapipe from reference\lavapipe; on this PC that tests the Intel driver itself).
# v13.28: dynubo + dynubo-copy (dynamic uniform buffer offsets; copy path used on feature level 11_0 GPUs).
# v13.26: areaclear entry (render pass clear limited to its render area; pawn atlas).
# v13.23: ssbowrite entry (GPU-written storage buffer must reach the CPU).
# v13.21b: ssbo instance 0 is yellow (was red on red, invisible); report says what each window should show.
# v13.21: clearattach now expects half red / half blue (rectangle clears); extra 'clearview' entry.
# v13.19: run the bridge regression suite on THIS PC's real GPU driver (and on Microsoft's
# WARP software rasterizer as a reference), without starting RimWorld. About 1-2 minutes.
# The same test program (tools\smoke.exe) runs in the developer's emulator; here it shows what
# the real Intel driver accepts or rejects.
param([switch]$NoWarp)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = Join-Path $root 'tools\smoke.exe'
$dll = Join-Path $root 'runtime\ashesD3D11Renderer.dll'
$reports = Join-Path $root 'reports'
if (-not (Test-Path $exe)) { throw 'tools\smoke.exe is missing.' }
New-Item -ItemType Directory -Force -Path $reports | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path $reports ('selftest-' + $stamp)
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Known-good frame-3 mean colours from the emulator (R,G,B). Real drivers may filter or
# rasterize edges slightly differently, so each channel may differ by up to $tol.
$expect = [ordered]@{
  'x'='209.1,45.9,0.0'; 'composite'='41.8,87.7,169.8'; 'push'='197.9,57.1,0.0'; 'loadpass'='209.1,45.9,0.0'
  'clearattach'='104.6,45.9,104.6'; 'hdrresolve'='209.1,45.9,0.0'; 'mipblit'='128.0,0.0,0.0'
  'swizzle'='41.8,41.8,169.8'; 'ssbo'='244.8,15.4,10.2'; 'pushubo'='255.0,8.0,8.0'
  'clearview'='104.6,45.9,104.6'; 'ssbowrite'='209.1,45.9,0.0'; 'areaclear'='104.6,22.9,127.5'; 'dynubo'='247.0,8.0,8.0'; 'dynubo-copy'='247.0,8.0,8.0'; 'vbopersist'='209.1,0.0,45.9'; 'varying'='209.1,0.0,45.9'; 'varyblock'='209.1,0.0,45.9'; 'varystruct'='209.1,0.0,45.9'; 'instuav'='232.0,0.0,22.9'; 'instuav-old'='237.7,0.0,17.2'; 'uavlayout'='232.0,0.0,22.9'; 'uavlayout-old'='254.9,0.0,0.0'; 'uavoffset'='232.0,0.0,22.9'; 'uavoffset-old'='254.9,0.0,0.0'; 'uavalias'='232.0,0.0,22.9'; 'uavalias-old'='232.0,0.0,22.9'
}
# What each test window should show (printed in the report, so a person can check by eye).
$looks = @{
  'x'='green triangle on red'; 'composite'='light-green triangle on blue (drawn offscreen, then sampled)'
  'push'='green triangle on red (position from push constants)'; 'loadpass'='green triangle on red (second pass keeps the first)'
  'clearattach'='left half blue, right half red, green triangle'; 'hdrresolve'='green triangle on red (float target copied to screen)'
  'mipblit'='plain dark red, no shape (half-size copy of a red/black pattern)'; 'swizzle'='dark-blue triangle on blue (grey texture expanded)'
  'ssbo'='four small triangles: yellow, green, blue, white'; 'pushubo'='one white triangle on the right'
  'clearview'='same as clearattach'; 'ssbowrite'='green triangle on red (a compute shader wrote numbers the CPU checked first)'; 'areaclear'='left half blue, right half red with half a green triangle'; 'dynubo'='small cyan triangle on the right'; 'dynubo-copy'='same as dynubo'; 'vbopersist'='BLUE triangle on red (vertex buffers bound before the pipeline; v13.33 colonist fix)'; 'varying'='BLUE triangle on red (pixel shader skips a vertex output; v13.35 plant fix) - green = bug'; 'varyblock'='BLUE triangle on red (interface block output, pixel shader reads member 1) - red = bug'; 'varystruct'='BLUE triangle on red (struct output like the game UI shader; v13.37 expansion-logo fix) - red = bug'; 'instuav'='TWO blue triangles, left and right (writable storage buffer in the vertex shader survives a rectangle clear; v13.43 forbidden-X fix) - right one cut off at the edge = bug'; 'instuav-old'='PROOF RUN with the v13.43 fix switched off (RW_GPU_UAV_RESTORE=0): must show the OLD BUG (only the left triangle; confirmed on HD 5500 and WARP in selftest-20261019-145657)'; 'uavlayout'='TWO blue triangles, left and right (instance buffer in set 1, sets bound with a sibling layout; v13.47 forbidden-X slot fix) - plain red = bug'; 'uavlayout-old'='PROOF RUN with the v13.47 fix off (RW_GPU_UAV_RESOLVE=0): must show the OLD BUG (plain red, no triangles)'; 'uavoffset'='TWO blue triangles (instance data at byte 256 of a shared buffer; v13.47b forbidden-X fix) - plain red = bug'; 'uavoffset-old'='PROOF RUN with v13.47b off (RW_GPU_UAV_RANGE=0): must show the OLD BUG (plain red)'; 'uavalias'='TWO blue triangles (instance offsets and colours in ONE storage buffer; v13.53 geyser-steam fix) - plain red/black or missing triangles = bug'; 'uavalias-old'='PROOF RUN with the v13.53 fix off (RW_GPU_UAV_ALIAS_COPY=0): on the HD 5500 and WARP a FAIL here (triangles missing or black) proves the bug; Wine does not enforce the D3D11 rule'
}
# Entries that run another smoke mode with a bridge option switched on.
# clearview: the same rectangle-clear test through the driver's D3D11.1 ClearView instead of
# the bridge's draw-based clear. Wine only stubs ClearView, so only real drivers can test it.
$variants = @{ 'clearview' = @{ Mode = 'clearattach'; Env = 'RW_GPU_RECTCLEAR'; Value = 'clearview' }; 'dynubo-copy' = @{ Mode = 'dynubo'; Env = 'RW_GPU_CB_EMULATE'; Value = '1' }; 'instuav-old' = @{ Mode = 'instuav'; Env = 'RW_GPU_UAV_RESTORE'; Value = '0' }; 'uavlayout-old' = @{ Mode = 'uavlayout'; Env = 'RW_GPU_UAV_RESOLVE'; Value = '0' }; 'uavoffset-old' = @{ Mode = 'uavoffset'; Env = 'RW_GPU_UAV_RANGE'; Value = '0' }; 'uavalias-old' = @{ Mode = 'uavalias'; Env = 'RW_GPU_UAV_ALIAS_COPY'; Value = '0' } }
$tol = 3.0
$devices = @(@{ Index = 0; Name = 'default GPU' })
if (-not $NoWarp) { $devices += @{ Index = 1; Name = 'WARP software reference' } }


# v13.41 water: smoke.exe reads its last frame back itself and prints an 8x6 grid of pixel colours,
# so the same numbers can be produced by any Vulkan driver. The reference is Mesa lavapipe
# (CPU Vulkan, reference\lavapipe, joined on first use). Tolerance: mean channel difference <= 4,
# at most 4 of the 48 samples more than 12 apart (edge pixels), and the picture must not be flat.
function Get-Grid([string]$text) {
  $m = [regex]::Match($text, '(?m)^readback grid:(.*)$')
  if (-not $m.Success) { return $null }
  $list = New-Object System.Collections.Generic.List[int[]]
  foreach ($p in ($m.Groups[1].Value.Trim() -split '\s+')) { $list.Add([int[]]($p -split ',' | ForEach-Object { [int]$_ })) }
  return $list
}
function Compare-Grid($a, $b) {
  if ($null -eq $a -or $null -eq $b -or $a.Count -ne $b.Count) { return $null }
  $d = New-Object System.Collections.Generic.List[int]
  for ($i = 0; $i -lt $a.Count; $i++) { $mx = 0; for ($c = 0; $c -lt 3; $c++) { $v = [Math]::Abs($a[$i][$c] - $b[$i][$c]); if ($v -gt $mx) { $mx = $v } }; $d.Add($mx) }
  $stats = $d | Measure-Object -Average -Maximum
  return @{ Mean = $stats.Average; Max = $stats.Maximum; Big = @($d | Where-Object { $_ -gt 12 }).Count }
}
function Get-Lavapipe {
  $lvpDir = Join-Path $root 'reference\lavapipe'
  $lvpDll = Join-Path $lvpDir 'vulkan_lvp.dll'
  if (-not (Test-Path -LiteralPath $lvpDir)) { return $null }
  if (-not (Test-Path -LiteralPath $lvpDll -PathType Leaf)) {
    $parts = @(Get-ChildItem -LiteralPath $lvpDir -Filter 'vulkan_lvp.dll.part*' | Sort-Object Name)
    if ($parts.Count -eq 0) { return $null }
    $out = [IO.File]::Create($lvpDll)
    try { foreach ($part in $parts) { $bytes = [IO.File]::ReadAllBytes($part.FullName); $out.Write($bytes, 0, $bytes.Length) } } finally { $out.Dispose() }
    $expected = (Get-Content -LiteralPath (Join-Path $lvpDir 'vulkan_lvp.dll.sha256') -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($expected -and (Get-FileHash -LiteralPath $lvpDll -Algorithm SHA256).Hash -ne $expected.Trim().ToUpper()) { Remove-Item -LiteralPath $lvpDll -Force; return $null }
  }
  return $lvpDll
}
function Run-Water([string]$icd, [string]$log, [string]$device) {
  if ($device) { $env:SMOKE_DEVICE = $device } else { Remove-Item Env:SMOKE_DEVICE -ErrorAction SilentlyContinue }
  $env:RW_HD5500_FRAME_DIAG = '1'
  $p = Start-Process -FilePath $exe -ArgumentList @("`"$icd`"", '4', 'water') -NoNewWindow -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
  $text = (Get-Content $log -Raw -ErrorAction SilentlyContinue) + (Get-Content ($log + '.err') -Raw -ErrorAction SilentlyContinue)
  $sd = [regex]::Match($text, 'lumaStddev=([0-9.,]+)')
  return @{ Grid = (Get-Grid $text); Stddev = $(if ($sd.Success) { [double]::Parse($sd.Groups[1].Value.Replace(',', '.'), [Globalization.CultureInfo]::InvariantCulture) } else { 0.0 }); Passed = ($text -match '(?m)^PASS'); Exit = $p.ExitCode }
}
$waterRef = $null
$lvp = Get-Lavapipe
if ($lvp) { $waterRef = Run-Water $lvp (Join-Path $work 'reference-lavapipe-water.txt') $null }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('HD5500 GPU Bridge laptop self-test ' + $stamp)
$lines.Add('Bridge: ' + (Get-Content (Join-Path $root 'VERSION.txt') -ErrorAction SilentlyContinue | Select-Object -First 1))
$failures = 0
$waterDev0 = $null
$verdicts = New-Object System.Collections.Generic.List[string]
foreach ($dev in $devices) {
  $devFailures = 0
  $lines.Add('')
  $lines.Add('== Device ' + $dev.Index + ' (' + $dev.Name + ') ==')
  foreach ($mode in $expect.Keys) {
    $log = Join-Path $work ("dev{0}-{1}.txt" -f $dev.Index, $mode)
    $dumpDir = Join-Path $work ("dev{0}-{1}-frames" -f $dev.Index, $mode)
    New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null
    $env:SMOKE_DEVICE = [string]$dev.Index
    $env:RW_HD5500_FRAME_DIAG = '1'
    $env:RW_HD5500_FRAME_DUMP_DIR = $dumpDir
    $env:RW_HD5500_D3D_DEBUG_LAYER = '1'   # Microsoft debug layer if installed (Graphics Tools); ignored otherwise
    $smokeMode = $mode
    # v13.53b: clear EVERY variant switch before each test. Only three were cleared before, so e.g.
    # uavoffset-old's RW_GPU_UAV_RANGE=0 leaked into uavalias, and device 0's last switches into the
    # whole WARP run (false FAILs of uavlayout/uavoffset/uavalias on WARP in selftest-20261019-230853).
    foreach ($name in @($variants.Values | ForEach-Object { $_.Env } | Sort-Object -Unique)) { Remove-Item -Path ('Env:' + $name) -ErrorAction SilentlyContinue }
    if ($variants.ContainsKey($mode)) {
      $smokeMode = $variants[$mode].Mode
      Set-Item -Path ('Env:' + $variants[$mode].Env) -Value $variants[$mode].Value
    }
    $p = Start-Process -FilePath $exe -ArgumentList @("`"$dll`"", '4', $smokeMode) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
    $text = (Get-Content $log -Raw -ErrorAction SilentlyContinue) + (Get-Content ($log + '.err') -Raw -ErrorAction SilentlyContinue)
    $adapter = [regex]::Match($text, 'device adapter="([^"]*)"').Groups[1].Value
    $m = [regex]::Match($text, 'dump frame=3 tag=swapchain.*?meanRGB=(\d+)[.,](\d),(\d+)[.,](\d),(\d+)[.,](\d)')
    $debugLines = ([regex]::Matches($text, '\[RW-D3D11-DEBUG\]')).Count
    $passed = $text -match '(?m)^PASS'
    if (-not $m.Success) {
      $lines.Add(('FAIL  {0,-12} no frame result (exit {1}){2}' -f $mode, $p.ExitCode, $(if ($text -match 'FAIL: (.*)') { ' ' + $Matches[1].Trim() } else { '' })))
      $failures++; $devFailures++; continue
    }
    $got = @( [double]("{0}.{1}" -f $m.Groups[1].Value, $m.Groups[2].Value), [double]("{0}.{1}" -f $m.Groups[3].Value, $m.Groups[4].Value), [double]("{0}.{1}" -f $m.Groups[5].Value, $m.Groups[6].Value) )
    $want = $expect[$mode].Split(',') | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) }
    $diff = [Math]::Max([Math]::Abs($got[0]-$want[0]), [Math]::Max([Math]::Abs($got[1]-$want[1]), [Math]::Abs($got[2]-$want[2])))
    $gotText = ($got | ForEach-Object { $_.ToString('0.0', [Globalization.CultureInfo]::InvariantCulture) }) -join ','
    $ok = $passed -and $diff -le $tol
    if (-not $ok) { $failures++; $devFailures++ }
    $lines.Add(('{0}  {1,-12} got={2,-18} expected={3,-18} debug-layer-msgs={4}  [should show: {5}]' -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $mode, $gotText, $expect[$mode], $debugLines, $looks[$mode]))
  }
  # v13.41 water (compared against lavapipe, or against the first device when lavapipe is missing)
  $w = Run-Water $dll (Join-Path $work ("dev{0}-water.txt" -f $dev.Index)) ([string]$dev.Index)
  if ($dev.Index -eq 0) { $waterDev0 = $w }
  $ref = $waterRef; $refName = 'lavapipe'
  if ($null -eq $ref -and $dev.Index -ne 0) { $ref = $waterDev0; $refName = 'default GPU' }
  if ($null -eq $w.Grid) {
    $lines.Add(('FAIL  {0,-12} no readback (exit {1}) [should show: rippled blue-grey water with a soft red border]' -f 'water', $w.Exit)); $failures++; $devFailures++
  } elseif ($null -eq $ref -or $null -eq $ref.Grid) {
    $lines.Add(('{0}  {1,-12} lumaStddev={2:0.0} (no reference to compare against)  [should show: rippled blue-grey water, NOT a flat colour]' -f $(if ($w.Passed -and $w.Stddev -gt 10) { 'ok  ' } else { 'FAIL' }), 'water', $w.Stddev))
    if (-not ($w.Passed -and $w.Stddev -gt 10)) { $failures++; $devFailures++ }
  } else {
    $c = Compare-Grid $w.Grid $ref.Grid
    $ok = $w.Passed -and $c.Mean -le 4 -and $c.Big -le 4 -and $w.Stddev -gt 10
    if (-not $ok) { $failures++; $devFailures++ }
    $lines.Add(('{0}  {1,-12} vs {2}: meanDiff={3:0.0} maxDiff={4} samplesOver12={5} lumaStddev={6:0.0}  [should show: rippled blue-grey water like the {2} window; flat = bug]' -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), 'water', $refName, $c.Mean, $c.Max, $c.Big, $w.Stddev))
  }
  if ($adapter) { $lines.Add('  adapter used: ' + $adapter) }
  $verdicts.Add(('  {0,-26} {1}' -f ($dev.Name + ':'), $(if ($devFailures -eq 0) { 'PASS (all ' + ($expect.Count + 1) + ' tests)' } else { 'FAIL (' + $devFailures + ' of ' + ($expect.Count + 1) + ')' })))
}
Remove-Item Env:SMOKE_DEVICE, Env:RW_GPU_RECTCLEAR, Env:RW_GPU_CB_EMULATE, Env:RW_GPU_UAV_RESTORE, Env:RW_HD5500_FRAME_DIAG, Env:RW_HD5500_FRAME_DUMP_DIR, Env:RW_HD5500_D3D_DEBUG_LAYER -ErrorAction SilentlyContinue
$lines.Add('')
$lines.Add('Result per GPU:')
$verdicts | ForEach-Object { $lines.Add($_) }
$lines.Add('(The default GPU line is the one that matters for playing. WARP is a slow software reference used to tell driver problems from bridge problems.)')
$lines.Add($(if ($failures -eq 0) { 'SELFTEST PASS' } else { "SELFTEST: $failures test(s) failed - see the per-GPU result above" }))
$lines | Set-Content (Join-Path $work 'SELFTEST.txt') -Encoding UTF8
$lines | Set-Content (Join-Path $reports 'LATEST-SELFTEST.txt') -Encoding UTF8
$lines | ForEach-Object { Write-Host $_ }
Write-Host ('Details: ' + $work)
