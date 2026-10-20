# v13.48 (+v13.61 RAM/Game Mode/background CPU, +v13.73 graphics/firmware/disk/Defender/Windows sections and suggestions): read-only PC check for the "freezes" (the CPU dropping to ~20% of its clock).
# Changes nothing. Writes reports\power-check-<time>\ with a battery report and a text summary.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $root ('reports\power-check-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $out | Out-Null
$txt = Join-Path $out 'power-check.txt'
function W([string]$s) { Add-Content -LiteralPath $txt -Value $s -Encoding UTF8; Write-Host $s }
W ('Power check ' + (Get-Date))
W '== Active power plan =='
(powercfg /getactivescheme) | ForEach-Object { W $_ }
W '== Processor min/max state on AC and battery (active plan) =='
(powercfg /query SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMAX) | Select-String 'Current AC|Current DC|Huidige|Index' | ForEach-Object { W ('max ' + $_.Line.Trim()) }
(powercfg /query SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN) | Select-String 'Current AC|Current DC|Huidige|Index' | ForEach-Object { W ('min ' + $_.Line.Trim()) }
W '== Battery and charger =='
try {
  Get-CimInstance Win32_Battery | ForEach-Object { W ('Battery: status=' + $_.BatteryStatus + ' (2 = on AC/charging) charge=' + $_.EstimatedChargeRemaining + '% name=' + $_.Name) }
  $full = Get-CimInstance -Namespace root\wmi -ClassName BatteryFullChargedCapacity -ErrorAction SilentlyContinue | Select-Object -First 1
  $design = Get-CimInstance -Namespace root\wmi -ClassName BatteryStaticData -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($full -and $design -and $design.DesignedCapacity) { W ('Battery health: full charge ' + $full.FullChargedCapacity + ' of design ' + $design.DesignedCapacity + ' mWh = ' + [Math]::Round(100.0 * $full.FullChargedCapacity / $design.DesignedCapacity) + '%') }
  $st = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($st) { W ('Power online (charger connected)=' + $st.PowerOnline + ' charging=' + $st.Charging + ' discharging=' + $st.Discharging + ' rate=' + $st.ChargeRate + '/' + $st.DischargeRate + ' mW voltage=' + $st.Voltage + ' mV') }
} catch { W ('battery query failed: ' + $_) }
W '== Temperature (ACPI thermal zones, may be missing) =='
try { Get-CimInstance -Namespace root\wmi -ClassName MSAcpi_ThermalZoneTemperature -ErrorAction Stop | ForEach-Object { W ($_.InstanceName + ': ' + [Math]::Round($_.CurrentTemperature / 10.0 - 273.15, 1) + ' C') } } catch { W 'no ACPI temperature available (needs admin or not exposed)' }
W '== CPU speed now (10 samples, 1 s apart; 100 = nominal clock) =='
for ($i = 0; $i -lt 10; $i++) {
  try { $p = Get-CimInstance Win32_PerfFormattedData_Counters_ProcessorInformation -Filter "Name='_Total'" -ErrorAction Stop; W ('  performance=' + $p.PercentProcessorPerformance + '% busy=' + $p.PercentProcessorTime + '%') }
  catch {
    # v13.73: some Windows installs (seen on this Dutch Windows 10) have no formatted performance class; fall back to the clock WMI reports
    try { $c = Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1; W ('  clock ' + $c.CurrentClockSpeed + ' of ' + $c.MaxClockSpeed + ' MHz (Windows WMI; the Throttle monitor measures more precisely)') } catch { W ('  counter failed: ' + $_.Exception.Message) }
    $i = 10
  }
  Start-Sleep -Seconds 1
}
# v13.61: other things that limit games on this kind of laptop (read-only).
W '== Memory (RAM) modules - the HD 5500 uses system RAM as video memory =='
try {
  $mods = @(Get-CimInstance Win32_PhysicalMemory -ErrorAction Stop)
  foreach ($m in $mods) { W ('  ' + $m.BankLabel + ' / ' + $m.DeviceLocator + ': ' + [Math]::Round($m.Capacity / 1GB, 1) + ' GB, ' + $m.Speed + ' MT/s, ' + $m.Manufacturer + ' ' + ([string]$m.PartNumber).Trim()) }
  if ($mods.Count -eq 1) { W '  -> ONE module = single-channel memory. Integrated graphics get about half the memory bandwidth; a second, matching module (dual channel) is the biggest hardware upgrade for this laptop: in GPU-bound games the HD 5500 runs about 38% slower in single channel (pcbuildadvisor.com). RimWorld here is mostly CPU-bound, so expect less, but the CPU also gains bandwidth.' }
  elseif ($mods.Count -ge 2) { W ('  -> ' + $mods.Count + ' modules: dual channel is possible (best when both have the same size and speed).') }
} catch { W ('  memory query failed: ' + $_.Exception.Message) }
W '== Intel graphics power plan (inside the active Windows power plan) =='
$ig = (powercfg /query SCHEME_CURRENT 44f3beca-a7c0-460e-9df2-bb8b99e0cba6 3619c3f2-afb2-4afc-b0e9-e7fef372de36 2>$null) | Select-String 'Current AC|Current DC|Huidige'
if ($ig) { foreach ($l in $ig) { W ('  ' + $l.Line.Trim() + '   (0 = max. battery life, 1 = balanced, 2 = maximum performance)') } } else { W '  not shown by Windows on this plan (hidden by "connected standby" or not provided by the driver)' }
W '== Windows Game Mode =='
try {
  $gm = Get-ItemProperty -Path 'HKCU:\Software\Microsoft\GameBar' -ErrorAction Stop
  $auto = $gm.PSObject.Properties['AutoGameModeEnabled']
  W ('  Game Mode: ' + $(if ($auto -and $auto.Value -eq 0) { 'OFF' } else { 'on (Windows default)' }) + '  (Settings > Gaming > Game Mode; helps minimum fps on 2-core CPUs, can make background video stutter)')
} catch { W '  Game Mode: on (Windows default, no setting stored)' }
W '== Process Lasso / power helpers =='
$pl = @(Get-Process -Name ProcessLasso, ProcessGovernor -ErrorAction SilentlyContinue)
W ('  Process Lasso running: ' + $(if ($pl.Count) { 'yes (' + (($pl | ForEach-Object { $_.ProcessName }) -join ', ') + ')' } else { 'no' }))
W '== Busiest background programs (CPU over 5 s; close what you do not need before playing) =='
try {
  $a = @{}; foreach ($p in Get-Process) { try { $a[$p.Id] = $p.TotalProcessorTime.TotalMilliseconds } catch { } }
  Start-Sleep -Seconds 5
  $rows = foreach ($p in Get-Process) { try { if ($a.ContainsKey($p.Id)) { [pscustomobject]@{ Name = $p.ProcessName; Ms = $p.TotalProcessorTime.TotalMilliseconds - $a[$p.Id] } } } catch { } }
  $cores = [Environment]::ProcessorCount
  $rows | Where-Object { $_.Name -ne 'Idle' } | Sort-Object Ms -Descending | Select-Object -First 8 | ForEach-Object { W ('  {0,-28} {1,5:N1}% of the CPU' -f $_.Name, (100.0 * $_.Ms / 5000.0 / $cores)) }
} catch { W ('  process sample failed: ' + $_.Exception.Message) }
# ---------------- v13.73: more read-only checks (graphics, firmware, disk, Defender, Windows) ----------------
$tips = New-Object System.Collections.Generic.List[string]
W '== Graphics adapters =='
try {
  $gpus = @(Get-CimInstance Win32_VideoController -ErrorAction Stop)
  foreach ($g in $gpus) { W ('  ' + $g.Name + '  driver ' + $g.DriverVersion + ' (' + $(if ($g.DriverDate) { ([datetime]$g.DriverDate).ToString('yyyy-MM-dd') } else { '?' }) + ')  status ' + $g.Status) }
  if ($gpus | Where-Object { $_.Name -match 'NVIDIA|GeForce|Radeon' }) { $tips.Add('A second graphics chip was found (see "Graphics adapters"). Some Z70-80 models have a GeForce 840M, which has its own Vulkan driver: RimWorld could then run WITHOUT the bridge. Tell Claude what is listed.') }
} catch { W ('  query failed: ' + $_.Exception.Message) }
W '== Laptop model, BIOS, thermal/power software =='
try {
  $prod = Get-CimInstance Win32_ComputerSystemProduct -ErrorAction Stop; $bios = Get-CimInstance Win32_BIOS -ErrorAction Stop
  W ('  model: ' + $prod.Vendor + ' ' + $prod.Version + ' (' + $prod.Name + ')')
  W ('  BIOS: ' + $bios.SMBIOSBIOSVersion + ' dated ' + $(if ($bios.ReleaseDate) { ([datetime]$bios.ReleaseDate).ToString('yyyy-MM-dd') } else { '?' }) + '  (newer BIOS/EC versions only from Lenovo''s support page for this serial number)')
} catch { W ('  query failed: ' + $_.Exception.Message) }
$apps = foreach ($k in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*') { Get-ItemProperty $k -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -match 'Energy Management|Lenovo Vantage|Lenovo Settings|Dynamic Platform|Thermal Framework|Intel.*Graphics|Intel.*Management Engine' } | ForEach-Object { '  ' + $_.DisplayName + ' ' + $_.DisplayVersion } }
if ($apps) { $apps | Sort-Object -Unique | ForEach-Object { W $_ } } else { W '  no Lenovo power/thermal program found in the installed-programs list' }
try {
  $dptf = @(Get-CimInstance Win32_PnPSignedDriver -ErrorAction Stop | Where-Object { $_.DeviceName -match 'Dynamic Platform|Thermal|DPTF' })
  foreach ($d in $dptf) { W ('  driver: ' + $d.DeviceName + ' ' + $d.DriverVersion) }
} catch { }
if ($apps -match 'Energy Management|Vantage|Lenovo Settings') { $tips.Add('Lenovo Energy Management/Vantage is installed: switch on "Battery conservation mode" (battery stays at ~55-60%, so the charger does not also heat the laptop while you play) and run "Dust removal" (fan at full speed for a minute). Both are switched off again in the same program.') }
else { $tips.Add('Lenovo Energy Management (ideapad) offers "Battery conservation mode" (less charging heat while playing) and "Dust removal". It is on Lenovo''s support page for the Z70-80.') }
W '== Firmware speed limits in the last 7 days (Windows event log, Kernel-Processor-Power event 37) =='
try {
  $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-Processor-Power'; Id = 37; StartTime = (Get-Date).AddDays(-7) } -ErrorAction Stop)
  W ('  ' + $ev.Count + ' events "speed limited by system firmware"; newest: ' + $(if ($ev.Count) { $ev[0].TimeCreated.ToString('yyyy-MM-dd HH:mm') + ' - ' + (($ev[0].Message -split "`n")[0]).Trim() } else { '-' }))
  if ($ev.Count) { $tips.Add('Windows logged ' + $ev.Count + ' "speed limited by system firmware" events this week: the slowdowns are ordered by the laptop''s firmware (heat, charger or battery), not by Windows or the game. Compare a run on the charger with a run on battery, and with the original Lenovo charger.') }
} catch { W '  none found (or the log is not readable without admin)' }
W '== Drives: game, bridge and caches =='
function DriveInfo([string]$letter, [string]$what) {
  try {
    $v = Get-Volume -DriveLetter $letter -ErrorAction Stop
    $bus = '?'; $media = '?'
    try { $d = Get-Partition -DriveLetter $letter -ErrorAction Stop | Get-Disk -ErrorAction Stop; $bus = $d.BusType; $pd = Get-PhysicalDisk -ErrorAction SilentlyContinue | Where-Object { $_.DeviceId -eq [string]$d.Number }; if ($pd) { $media = $pd.MediaType } } catch { }
    $freePct = if ($v.Size) { [Math]::Round(100.0 * $v.SizeRemaining / $v.Size) } else { 0 }
    W ('  ' + $letter + ': (' + $what + ') ' + $v.FileSystem + ', ' + [Math]::Round($v.SizeRemaining / 1GB, 1) + ' GB free of ' + [Math]::Round($v.Size / 1GB, 1) + ' GB (' + $freePct + '%), bus ' + $bus + ', media ' + $media)
    return $freePct
  } catch { W ('  ' + $letter + ': (' + $what + ') query failed: ' + $_.Exception.Message); return 100 }
}
$cFree = DriveInfo 'C' 'Windows, RimWorld (Steam), shader caches'
$bridgeLetter = (Split-Path -Qualifier $root).TrimEnd(':')
if ($bridgeLetter -and $bridgeLetter -ne 'C') { DriveInfo $bridgeLetter 'bridge, logs, frame pictures' | Out-Null }
if ($cFree -lt 15) { $tips.Add('C: has only ' + $cFree + '% free. SSDs slow down when nearly full and the shader caches and page file need room: aim for 15-20% free (Settings > System > Storage; in Disk Cleanup leave "DirectX Shader Cache" UNticked, deleting it makes the next game start recompile every shader).') }
$trim = (fsutil behavior query DisableDeleteNotify 2>$null) -join ' '
W ('  TRIM (SSD housekeeping): ' + $trim + '   (0 = on, good)')
foreach ($c in @(@{ n = 'DirectX shader cache'; p = (Join-Path $env:LOCALAPPDATA 'D3DSCache') }, @{ n = 'Intel shader cache'; p = (Join-Path $env:USERPROFILE 'AppData\LocalLow\Intel\ShaderCache') }, @{ n = 'Bridge shader cache'; p = (Join-Path $env:LOCALAPPDATA 'HD5500-GPU-Bridge\shadercache') })) {
  if (Test-Path -LiteralPath $c.p) { $sz = (Get-ChildItem -LiteralPath $c.p -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum; W ('  ' + $c.n + ': ' + [Math]::Round($sz / 1MB, 1) + ' MB  ' + $c.p) } else { W ('  ' + $c.n + ': not present  ' + $c.p) }
}
try { Get-CimInstance Win32_PageFileUsage -ErrorAction Stop | ForEach-Object { W ('  page file ' + $_.Name + ': ' + $_.AllocatedBaseSize + ' MB, peak use ' + $_.PeakUsage + ' MB (keep it "system managed")') } } catch { }
W '== Microsoft Defender (virus scanner) =='
$gameDir = 'C:\Program Files (x86)\Steam2\steamapps\common\RimWorld'
try {
  $mp = Get-MpComputerStatus -ErrorAction Stop
  W ('  real-time protection: ' + $(if ($mp.RealTimeProtectionEnabled) { 'on' } else { 'off' }))
  $pref = Get-MpPreference -ErrorAction Stop
  $ex = @($pref.ExclusionPath) | Where-Object { $_ }
  if ($ex -and ($ex -join ' ') -match 'N/A') { W '  exclusions: only visible to an administrator' } elseif ($ex) { $ex | ForEach-Object { W ('  excluded: ' + $_) } } else { W '  exclusions: none' }
  if ($mp.RealTimeProtectionEnabled) { $tips.Add('Optional (your decision - it is a security setting): Defender scans every file the game and the shader caches open. Excluding only these folders can shorten loading and first-time stutter; no measured number exists for this laptop. Scan them once first. Admin PowerShell to add:  Add-MpPreference -ExclusionPath "' + $gameDir + '","' + (Join-Path $env:LOCALAPPDATA 'D3DSCache') + '","' + (Join-Path $env:LOCALAPPDATA 'HD5500-GPU-Bridge') + '"   to undo: the same line with Remove-MpPreference.') }
} catch { W ('  Defender status not readable: ' + $_.Exception.Message) }
W '== Windows: Game Bar recording, visual effects, Turbo, search =='
$dvr = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GameDVR' -ErrorAction SilentlyContinue
$cap = if ($dvr -and $null -ne $dvr.AppCaptureEnabled) { [int]$dvr.AppCaptureEnabled } else { -1 }
$bg = if ($dvr -and $null -ne $dvr.HistoricalCaptureEnabled) { [int]$dvr.HistoricalCaptureEnabled } else { 0 }
W ('  Game Bar captures: ' + $(if ($cap -eq 0) { 'off' } elseif ($cap -eq 1) { 'on' } else { 'Windows default' }) + ', background recording: ' + $(if ($bg -eq 1) { 'ON' } else { 'off' }))
if ($bg -eq 1) { $tips.Add('Game Bar "Record in the background" is ON: it records the last minutes of every game and costs CPU/GPU on slow PCs. Menu: Laptop health > "Game Bar recording off" (undo button next to it).') }
$fx = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects' -ErrorAction SilentlyContinue
$tr = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -ErrorAction SilentlyContinue
W ('  visual effects: ' + $(switch ([int]$fx.VisualFXSetting) { 1 { 'best appearance' } 2 { 'best performance' } 3 { 'custom' } default { 'let Windows choose' } }) + ', transparency: ' + $(if ($tr -and $tr.EnableTransparency -eq 0) { 'off' } else { 'on' }))
$boost = (powercfg /query SCHEME_CURRENT SUB_PROCESSOR PERFBOOSTMODE 2>$null) | Select-String 'Current AC|Huidige' | Select-Object -First 1
W ('  Turbo (processor boost mode, AC): ' + $(if ($boost) { $boost.Line.Trim() + '  (0 = Turbo off, 2 = aggressive)' } else { 'hidden in this plan (Turbo on, Windows default)' }))
$ws = Get-Service WSearch -ErrorAction SilentlyContinue
W ('  Windows Search indexer: ' + $(if ($ws) { $ws.Status } else { 'not installed' }))
$tips.Add('Throttling test worth one evening: menu Laptop health > "Cool mode (Turbo off)". It makes a copy of your power plan with the CPU capped at its 2.2 GHz base clock (no 2.7 GHz Turbo). Peak speed drops ~12%, but heat drops a lot; on 15 W laptops that often avoids the 22% throttle cycles. Compare the 1% lows of a 10-minute Speed test with and without; "Undo cool mode" restores your plan.')
$tips.Add('Intel graphics control panel > Power > Plugged in: "Maximum Performance". The CPU is the bottleneck here, so expect little; it costs nothing to try (Restore Defaults undoes it).')
W ''
W '== Suggestions (nothing has been changed; each one can be undone) =='
$n = 0; foreach ($t in $tips) { $n++; W ('  ' + $n + '. ' + $t) }
W ''
W '== Battery report (HTML, Windows built-in) =='
powercfg /batteryreport /output (Join-Path $out 'battery-report.html') | Out-Null
W ('saved ' + (Join-Path $out 'battery-report.html'))
W 'Done. Nothing was changed.'
