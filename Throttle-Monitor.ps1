# v13.50 Throttle monitor - our own live view of the laptop's speed, heat and power while RimWorld
# runs through the bridge. Read-only: it changes no processor or Windows setting. The bridge writes
# reports\live-power.txt once per second; this window shows it plus the battery status from Windows.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$live = Join-Path $root 'reports\live-power.txt'
$form = New-Object System.Windows.Forms.Form
$form.Text = 'HD5500 Bridge - Throttle monitor'
$form.ClientSize = New-Object System.Drawing.Size(460, 420)
$form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false; $form.TopMost = $true
$font = New-Object System.Drawing.Font('Consolas', 10)
$big = New-Object System.Drawing.Font('Segoe UI', 12, [System.Drawing.FontStyle]::Bold)
$head = New-Object System.Windows.Forms.Label
$head.Location = New-Object System.Drawing.Point(12, 10); $head.Size = New-Object System.Drawing.Size(436, 26); $head.Font = $big
$form.Controls.Add($head)
$body = New-Object System.Windows.Forms.Label
$body.Location = New-Object System.Drawing.Point(12, 42); $body.Size = New-Object System.Drawing.Size(436, 200); $body.Font = $font
$form.Controls.Add($body)
$graphLabel = New-Object System.Windows.Forms.Label
$graphLabel.Location = New-Object System.Drawing.Point(12, 246); $graphLabel.Size = New-Object System.Drawing.Size(436, 18)
$graphLabel.Text = 'CPU speed, last 2 minutes (green = full speed, red = throttled)'
$form.Controls.Add($graphLabel)
$graph = New-Object System.Windows.Forms.PictureBox
$graph.Location = New-Object System.Drawing.Point(12, 266); $graph.Size = New-Object System.Drawing.Size(436, 80); $graph.BackColor = [System.Drawing.Color]::Black
$form.Controls.Add($graph)
$verdict = New-Object System.Windows.Forms.Label
$verdict.Location = New-Object System.Drawing.Point(12, 352); $verdict.Size = New-Object System.Drawing.Size(436, 60)
$form.Controls.Add($verdict)
$history = New-Object System.Collections.Generic.List[int]
$script:lastStamp = ''
$script:staleSince = $null

function Read-Live {
    if (-not (Test-Path -LiteralPath $live)) { return $null }
    $h = @{}
    try { foreach ($l in [IO.File]::ReadAllLines($live)) { $i = $l.IndexOf('='); if ($i -gt 0) { $h[$l.Substring(0, $i)] = $l.Substring($i + 1) } } } catch { return $null }
    return $h
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1000
$timer.Add_Tick({
    $p = [System.Windows.Forms.SystemInformation]::PowerStatus
    $charger = if ($p.PowerLineStatus -eq 'Online') { 'connected' } else { 'NOT connected' }
    $batt = [Math]::Round($p.BatteryLifePercent * 100)
    $h = Read-Live
    $running = $false
    if ($h -and $h['stamp'] -ne $script:lastStamp) { $script:lastStamp = $h['stamp']; $script:staleSince = $null; $running = $true }
    elseif ($h) { if (-not $script:staleSince) { $script:staleSince = Get-Date }; $running = ((Get-Date) - $script:staleSince).TotalSeconds -lt 4 }
    if (-not $running) {
        $head.Text = 'Waiting for RimWorld (start it from the Bridge menu)'; $head.ForeColor = [System.Drawing.Color]::DimGray
        $body.Text = "Charger: $charger`r`nBattery: $batt%  ($($p.BatteryChargeStatus))"
        return
    }
    $ratio = [int]$h['probe']
    $speedPct = if ($ratio -gt 0) { [Math]::Round(10000.0 / $ratio) } else { 100 }   # 100 = as fast as its best moment
    $throttled = $h['throttled'] -eq '1'
    $history.Add([int]$speedPct); while ($history.Count -gt 120) { $history.RemoveAt(0) }
    $head.Text = if ($throttled) { 'THROTTLED - the CPU is running slow' } else { 'Running at full speed' }
    $head.ForeColor = if ($throttled) { [System.Drawing.Color]::Firebrick } else { [System.Drawing.Color]::ForestGreen }
    $temp = if ($h['temp'] -like '-*') { 'not exposed by this laptop' } else { $h['temp'] + ' C' }
    $limit = if ([int]$h['limit'] -ge 10) { '-' + $h['limit'] + '% (firmware is holding the CPU back)' } else { 'none' }
    $guard = switch ($h['guard']) { 'active' { 'ACTIVE - frame rate capped to cool down' } 'armed' { 'armed (waits for throttling or heat)' } default { 'off (Bridge menu option)' } }
    $body.Text = ("Frames per second : {0}`r`nCPU speed (probe) : {1}% of its best`r`nWindows CPU speed : {2}%  clock {3} MHz`r`nFirmware limit    : {4}`r`nTemperature       : {5}`r`nCharger           : {6}`r`nBattery           : {7}%  ({8})`r`nHeat guard        : {9}`r`nThrottled so far  : {10} s (guard engaged {11}x)" -f `
        $h['fps'], $speedPct, $h['speed'], $h['mhz'], $limit, $temp, $charger, $batt, $p.BatteryChargeStatus, $guard, $h['throttledSeconds'], $h['guardEngagements'])
    # graph
    $bmp = New-Object System.Drawing.Bitmap(436, 80)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::Black)
    $w = 436.0 / 120.0
    for ($i = 0; $i -lt $history.Count; $i++) {
        $v = [Math]::Min(100, [Math]::Max(0, $history[$i])); $hh = [int](78 * $v / 100.0)
        $brush = if ($v -lt 40) { [System.Drawing.Brushes]::Red } elseif ($v -lt 70) { [System.Drawing.Brushes]::Orange } else { [System.Drawing.Brushes]::LimeGreen }
        $g.FillRectangle($brush, [int]($i * $w), 79 - $hh, [Math]::Max(1, [int]$w), $hh)
    }
    $g.Dispose(); if ($graph.Image) { $graph.Image.Dispose() }; $graph.Image = $bmp
    # verdict
    $t = [int]$h['throttledSeconds']
    if ($t -eq 0) { $verdict.Text = 'No throttling seen in this session.' }
    elseif ($h['temp'] -notlike '-*' -and [int]$h['temp'] -ge 85) { $verdict.Text = 'Throttling while hot: cooling is the limit (fan, dust, thermal paste). The heat guard option lowers the load.' }
    elseif ($p.PowerLineStatus -ne 'Online') { $verdict.Text = 'Running on battery: Windows and the battery limit the speed. Connect the charger.' }
    else { $verdict.Text = 'Throttling with the charger connected and no high temperature reading: typical of a worn battery / power limit (Lenovo BD PROCHOT). See TESTING.md.' }
})
$timer.Start()
[void]$form.ShowDialog()
