# HD5500 GPU Bridge menu.
# v13.19: first graphical menu. v13.83: Local build page. v13.84: tick-box switch editor. v13.85: presets. v13.29: lavapipe reference. v13.43: auto-close. v13.50: laptop speed/heat.
# v13.54: redesigned: dark mode (default; Light/Dark toggle remembered in Bridge-Menu.settings.json),
#         sidebar navigation (Home, Play, Tests, Diagnostics, Laptop health, Results, Options, Help),
#         every action shows a one-line description, and an explanation panel at the bottom shows the
#         full text for whatever the mouse or keyboard focus is on (no hidden tooltips). Home shows the
#         latest frame-rate numbers (average, 1% low, 0.1% low) and the latest self-test result.
#         Keyboard: Ctrl+1..8 switch pages, F5 refreshes results, Tab moves through the actions.
# Start with Bridge-Menu.cmd. Windows PowerShell 5.1 compatible.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# ---------- photosensitivity warning (shown every time the menu starts) ----------
$warning = @"
PHOTOSENSITIVE SEIZURE WARNING

This is experimental graphics software. While testing, the game window can flicker, flash, show rapidly changing colours or patterns, or switch between black and bright frames.

A very small number of people may experience seizures when exposed to such images, even without any history of epilepsy. If you or anyone in your family has epilepsy or has had seizures, consult a doctor before use.

Stop immediately and close the game if you notice dizziness, altered vision, eye or muscle twitching, loss of awareness, disorientation or any involuntary movement.

Reduce the risk: play in a well-lit room, sit further from the screen, use a smaller window, take regular breaks, and do not watch unattended tests while they run.

Click OK to continue, or Cancel to close the menu.
"@
$answer = [System.Windows.Forms.MessageBox]::Show($warning, 'HD5500 GPU Bridge - health warning', [System.Windows.Forms.MessageBoxButtons]::OKCancel, [System.Windows.Forms.MessageBoxIcon]::Warning)
if ($answer -ne [System.Windows.Forms.DialogResult]::OK) { return }

$root = $PSScriptRoot
$reports = Join-Path $root 'reports'
$version = Get-Content (Join-Path $root 'VERSION.txt') -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $version) { $version = 'version unknown' }

# ---------- settings (theme + option checkboxes), remembered between starts ----------
$settingsFile = Join-Path $root 'Bridge-Menu.settings.json'
$settings = @{ theme = 'dark'; options = @{} }
try {
    if (Test-Path -LiteralPath $settingsFile) {
        $loaded = Get-Content -LiteralPath $settingsFile -Raw | ConvertFrom-Json
        if ($loaded.theme) { $settings.theme = [string]$loaded.theme }
        if ($loaded.options) { foreach ($p in $loaded.options.PSObject.Properties) { $settings.options[$p.Name] = [bool]$p.Value } }
    }
} catch { }
$script:optionBoxes = @{}
function Save-Settings {
    try {
        $opts = @{}
        foreach ($k in $script:optionBoxes.Keys) { $opts[$k] = $script:optionBoxes[$k].Checked }
        @{ theme = $script:settings.theme; options = $opts } | ConvertTo-Json | Set-Content -LiteralPath $script:settingsFile -Encoding UTF8
    } catch { }
}

# ---------- colours ----------
$themes = @{
    dark  = @{ Back = [System.Drawing.Color]::FromArgb(30, 31, 34); Side = [System.Drawing.Color]::FromArgb(22, 23, 25); Card = [System.Drawing.Color]::FromArgb(43, 45, 49)
               CardHover = [System.Drawing.Color]::FromArgb(56, 59, 64); Text = [System.Drawing.Color]::FromArgb(230, 230, 230); Muted = [System.Drawing.Color]::FromArgb(160, 163, 170)
               Accent = [System.Drawing.Color]::FromArgb(0, 103, 192); AccentText = [System.Drawing.Color]::White; Border = [System.Drawing.Color]::FromArgb(70, 73, 78)
               Warn = [System.Drawing.Color]::FromArgb(240, 180, 80); Heading = [System.Drawing.Color]::FromArgb(110, 180, 255) }
    light = @{ Back = [System.Drawing.Color]::FromArgb(245, 246, 248); Side = [System.Drawing.Color]::FromArgb(228, 231, 236); Card = [System.Drawing.Color]::White
               CardHover = [System.Drawing.Color]::FromArgb(232, 240, 252); Text = [System.Drawing.Color]::FromArgb(25, 25, 28); Muted = [System.Drawing.Color]::FromArgb(95, 99, 106)
               Accent = [System.Drawing.Color]::FromArgb(0, 102, 204); AccentText = [System.Drawing.Color]::White; Border = [System.Drawing.Color]::FromArgb(200, 204, 210)
               Warn = [System.Drawing.Color]::FromArgb(170, 100, 0); Heading = [System.Drawing.Color]::FromArgb(0, 102, 204) }
}
if (-not $themes.ContainsKey($settings.theme)) { $settings.theme = 'dark' }

# Dark Windows title bar (Windows 10 20H1+ / 11). Harmless if unavailable.
try {
    Add-Type -Namespace BridgeMenu -Name Dwm -MemberDefinition '[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);' -ErrorAction Stop
    $script:dwmOk = $true
} catch { $script:dwmOk = $false }
function Set-TitleBar([System.Windows.Forms.Form]$f, [bool]$dark) {
    if (-not $script:dwmOk) { return }
    try { $v = [int]$dark; [void][BridgeMenu.Dwm]::DwmSetWindowAttribute($f.Handle, 20, [ref]$v, 4); [void][BridgeMenu.Dwm]::DwmSetWindowAttribute($f.Handle, 19, [ref]$v, 4) } catch { }
}

$fontBase = New-Object System.Drawing.Font('Segoe UI', 9.5)
$fontBold = New-Object System.Drawing.Font('Segoe UI Semibold', 10)
$fontTitle = New-Object System.Drawing.Font('Segoe UI Semibold', 15)
$fontPage = New-Object System.Drawing.Font('Segoe UI Semibold', 13)
$fontSmall = New-Object System.Drawing.Font('Segoe UI', 8.5)

# ---------- window skeleton ----------
$form = New-Object System.Windows.Forms.Form
$form.Text = 'HD5500 GPU Bridge'
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(940, 660)
$form.MinimumSize = New-Object System.Drawing.Size(880, 620)
$form.Font = $fontBase
$form.KeyPreview = $true

$side = New-Object System.Windows.Forms.Panel
$side.Dock = 'Left'; $side.Width = 200; $side.Padding = New-Object System.Windows.Forms.Padding(10, 12, 10, 10)
$main = New-Object System.Windows.Forms.Panel
$main.Dock = 'Fill'; $main.Padding = New-Object System.Windows.Forms.Padding(22, 16, 22, 8)
$bottom = New-Object System.Windows.Forms.Panel
$bottom.Dock = 'Bottom'; $bottom.Height = 140; $bottom.Padding = New-Object System.Windows.Forms.Padding(22, 8, 22, 8)
$form.Controls.Add($main); $form.Controls.Add($bottom); $form.Controls.Add($side)

# Explanation panel (bottom): full text of the hovered / focused item.
$detailTitle = New-Object System.Windows.Forms.Label
$detailTitle.Dock = 'Top'; $detailTitle.Height = 22; $detailTitle.Font = $fontBold; $detailTitle.Text = 'Explanation'
$detail = New-Object System.Windows.Forms.Label
$detail.Dock = 'Fill'; $detail.Text = 'Move the mouse over a button or option (or move to it with the Tab key) to see what it does.'
$status = New-Object System.Windows.Forms.Label
$status.Dock = 'Bottom'; $status.Height = 36; $status.Font = $fontSmall
$bottom.Controls.Add($detail); $bottom.Controls.Add($detailTitle); $bottom.Controls.Add($status)
function Show-Detail([string]$title, [string]$text) { $script:detailTitle.Text = $title; $script:detail.Text = $text }

# Sidebar: app title, version, navigation, theme toggle.
$appTitle = New-Object System.Windows.Forms.Label
$appTitle.Text = "HD5500`nGPU Bridge"; $appTitle.Font = $fontTitle; $appTitle.Dock = 'Top'; $appTitle.Height = 62
$appVer = New-Object System.Windows.Forms.Label
$appVer.Text = ($version -replace '\s*\(renderer [0-9A-F]+\)', ''); $appVer.Font = $fontSmall; $appVer.Dock = 'Top'; $appVer.Height = 30
$navHost = New-Object System.Windows.Forms.FlowLayoutPanel
$navHost.Dock = 'Fill'; $navHost.FlowDirection = 'TopDown'; $navHost.WrapContents = $false; $navHost.Padding = New-Object System.Windows.Forms.Padding(0, 6, 0, 0)
$themeButton = New-Object System.Windows.Forms.Button
$themeButton.Dock = 'Bottom'; $themeButton.Height = 32; $themeButton.FlatStyle = 'Flat'
$side.Controls.Add($navHost); $side.Controls.Add($themeButton); $side.Controls.Add($appVer); $side.Controls.Add($appTitle)

# ---------- page builders ----------
$pages = [ordered]@{}
$navButtons = @{}

function New-Page([string]$key, [string]$navText, [string]$heading, [string]$intro) {
    $p = New-Object System.Windows.Forms.Panel
    $p.Dock = 'Fill'; $p.AutoScroll = $true; $p.Visible = $false
    $h = New-Object System.Windows.Forms.Label
    $h.Text = $heading; $h.Font = $script:fontPage; $h.AutoSize = $false; $h.Location = New-Object System.Drawing.Point(0, 0); $h.Size = New-Object System.Drawing.Size(690, 30)
    $i = New-Object System.Windows.Forms.Label
    $i.Text = $intro; $i.AutoSize = $false; $i.Location = New-Object System.Drawing.Point(0, 32); $i.Size = New-Object System.Drawing.Size(690, 44); $i.Tag = 'muted'
    $p.Controls.Add($h); $p.Controls.Add($i)
    $p.Tag = @{ NextY = 84 }
    $script:main.Controls.Add($p)
    $script:pages[$key] = $p
    $b = New-Object System.Windows.Forms.Button
    $b.Text = '   ' + $navText; $b.TextAlign = 'MiddleLeft'; $b.FlatStyle = 'Flat'; $b.Size = New-Object System.Drawing.Size(178, 36); $b.Tag = $key
    $b.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, 4)
    $b.Add_Click({ Show-Page $this.Tag })
    $b.Add_MouseEnter({ Show-Detail $this.Text.Trim() ('Opens the "' + $this.Text.Trim() + '" page. Tip: Ctrl+1..8 switch pages from the keyboard.') })
    $script:navHost.Controls.Add($b)
    $script:navButtons[$key] = $b
    return $p
}

# One action row: a button with a short title, a one-line description next to it, and the full
# explanation in the bottom panel on hover/focus.
function Add-Action($page, [string]$text, [string]$short, [string]$help, [scriptblock]$action, [bool]$primary = $false) {
    $y = $page.Tag.NextY
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.TextAlign = 'MiddleLeft'; $b.FlatStyle = 'Flat'; $b.Font = $script:fontBold
    $b.Location = New-Object System.Drawing.Point(0, $y); $b.Size = New-Object System.Drawing.Size(300, 40)
    $b.Padding = New-Object System.Windows.Forms.Padding(8, 0, 0, 0)
    $b.Tag = @{ Kind = $(if ($primary) { 'primary' } else { 'action' }); Help = $help; Title = $text }
    $b.Add_Click($action)
    $b.Add_MouseEnter({ Show-Detail $this.Tag.Title $this.Tag.Help })
    $b.Add_GotFocus({ Show-Detail $this.Tag.Title $this.Tag.Help })
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $short; $l.AutoSize = $false; $l.Location = New-Object System.Drawing.Point(312, ($y + 1)); $l.Size = New-Object System.Drawing.Size(380, 38); $l.Tag = 'muted'
    $l.TextAlign = 'MiddleLeft'
    $page.Controls.Add($b); $page.Controls.Add($l)
    $page.Tag.NextY = $y + 48
    return $b
}

function Add-Option($page, [string]$key, [string]$text, [string]$short, [string]$help, [bool]$default = $false) {
    $y = $page.Tag.NextY
    $c = New-Object System.Windows.Forms.CheckBox
    $c.Text = $text; $c.Font = $script:fontBold; $c.AutoSize = $false; $c.Location = New-Object System.Drawing.Point(0, $y); $c.Size = New-Object System.Drawing.Size(690, 24)
    $c.FlatStyle = 'Flat'
    if ($script:settings.options.ContainsKey($key)) { $c.Checked = $script:settings.options[$key] } else { $c.Checked = $default }
    $c.Tag = @{ Kind = 'option'; Help = $help; Title = $text }
    $c.Add_MouseEnter({ Show-Detail $this.Tag.Title $this.Tag.Help })
    $c.Add_GotFocus({ Show-Detail $this.Tag.Title $this.Tag.Help })
    $c.Add_CheckedChanged({ Save-Settings; Update-OptionSummary })
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $short; $l.AutoSize = $false; $l.Location = New-Object System.Drawing.Point(22, ($y + 24)); $l.Size = New-Object System.Drawing.Size(668, 20); $l.Tag = 'muted'
    $page.Controls.Add($c); $page.Controls.Add($l)
    $script:optionBoxes[$key] = $c
    $page.Tag.NextY = $y + 52
    return $c
}

function Add-Note($page, [string]$text, [int]$height = 40) {
    $y = $page.Tag.NextY
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.AutoSize = $false; $l.Location = New-Object System.Drawing.Point(0, $y); $l.Size = New-Object System.Drawing.Size(690, $height); $l.Tag = 'muted'
    $page.Controls.Add($l)
    $page.Tag.NextY = $y + $height + 6
    return $l
}

function Add-Heading($page, [string]$text) {
    $y = $page.Tag.NextY + 4
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.Font = $script:fontBold; $l.AutoSize = $false; $l.Location = New-Object System.Drawing.Point(0, $y); $l.Size = New-Object System.Drawing.Size(690, 22); $l.Tag = 'accent'
    $page.Controls.Add($l)
    $page.Tag.NextY = $y + 26
}

# ---------- launching (same behaviour as the previous menu) ----------
function Get-GameArgs([string]$extra) {
    $a = @()
    if ($script:chkNoAtlas.Checked) { $a += '-noCompressAtlases' }
    if ($script:chkReset.Checked) { $a += '-ResetVulkanFallback' }
    if ($extra) { $a += $extra }
    return ($a -join ' ')
}
function Set-OptionEnv {
    if ($script:chkWarp.Checked) { $env:WAYLITH_VULKAN_DEVICE = 'Basic Render' } else { Remove-Item Env:WAYLITH_VULKAN_DEVICE -ErrorAction SilentlyContinue }
    if ($script:chkDebug.Checked) { $env:RW_HD5500_D3D_DEBUG_LAYER = '1' } else { Remove-Item Env:RW_HD5500_D3D_DEBUG_LAYER -ErrorAction SilentlyContinue }
    if ($script:chkAutoClose.Checked) { $env:RW_BRIDGE_AUTOCLOSE = '1' } else { Remove-Item Env:RW_BRIDGE_AUTOCLOSE -ErrorAction SilentlyContinue }
    if ($script:chkGuard.Checked) { $env:RW_GPU_THERMAL_GUARD = '1' } else { Remove-Item Env:RW_GPU_THERMAL_GUARD -ErrorAction SilentlyContinue }
    if ($script:chkCap45.Checked) { $env:RW_GPU_FPS_CAP = '45' } else { Remove-Item Env:RW_GPU_FPS_CAP -ErrorAction SilentlyContinue }   # v13.71
    if ($script:chkPriority.Checked) { $env:RW_GPU_PROCESS_PRIORITY = 'above' } else { Remove-Item Env:RW_GPU_PROCESS_PRIORITY -ErrorAction SilentlyContinue }   # v13.61
    if ($script:chkSubmit.Checked) { $env:RW_GPU_SUBMIT_THREAD = '1' } else { Remove-Item Env:RW_GPU_SUBMIT_THREAD -ErrorAction SilentlyContinue }   # v13.61
}
function Start-Launcher([string]$label, [string]$switches, [string]$gameExtra, [string]$summaryLabel) {
    Set-OptionEnv
    $sw = $switches
    if ($script:chkFrames.Checked -and $sw -notmatch '-FrameDiag') { $sw += ' -FrameDiag' }
    if ($script:chkShaders.Checked -and $sw -notmatch '-DumpShaders') { $sw += ' -DumpShaders' }
    if ($script:chkTicks.Checked -and $sw -notmatch '-TickCounter') { $sw += ' -TickCounter' }
    $ga = Get-GameArgs $gameExtra
    $launch = Join-Path $script:root 'Launch-HD5500-GPU.ps1'
    $sum = Join-Path $script:root 'Summarize-Run.ps1'
    $cmd = "title $label & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$launch`" $sw -GameArguments `"$ga`""
    if ($summaryLabel) { $cmd += " & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$sum`" -Label `"$summaryLabel`"" }
    if ($script:chkAutoClose.Checked) { $cmd += ' & echo. & echo Finished. This window closes in 8 seconds. & timeout /t 8' }
    else { $cmd += ' & echo. & echo Finished. You can close this window. & pause' }
    Start-Process -FilePath 'cmd.exe' -ArgumentList @('/c', $cmd) -WorkingDirectory $script:root
    $script:status.Text = "Started: $label   (game arguments: $ga)"
}
function Start-Cmd([string]$file, [string]$label) {
    Set-OptionEnv
    Start-Process -FilePath 'cmd.exe' -ArgumentList @('/c', "`"$(Join-Path $script:root $file)`"") -WorkingDirectory $script:root
    $script:status.Text = "Started: $label"
}
function Open-Text([string]$path, [string]$missing) {
    if (Test-Path -LiteralPath $path) { Start-Process notepad.exe $path } else { $script:status.Text = $missing }
}

# ---------- Home ----------
$pHome = New-Page 'home' 'Home' 'RimWorld 1.7 through the HD5500 GPU Bridge' 'The bridge lets RimWorld''s Vulkan renderer run on this laptop''s Intel HD 5500 by translating it to Direct3D 11. Start playing below, or use the pages on the left for tests and diagnostics.'
Add-Action $pHome 'Play RimWorld' 'Normal play through the bridge (diagnostics off).' 'Starts RimWorld through the bridge with your settings from the Options page. Diagnostics are off, so it runs as fast as possible. A summary of the run (frame rate, 1% low, stutters) is written to reports\LATEST-SUMMARY.txt when the game closes.' { Start-Launcher 'Play RimWorld' '-QuickStart -SwapchainImages 1' '' 'play' } $true | Out-Null
Add-Action $pHome 'Map test (5 min, unattended)' 'Small test map, saves pictures and logs, closes by itself.' 'Opens the developer quicktest map, saves frame pictures and translated shaders, and closes after 5 minutes. Do not click in the game while it runs. This is the standard way to check a new bridge version.' { Start-Launcher 'Map test' '-QuickStart -SwapchainImages 1 -FrameDiag -DumpShaders -AutoCloseSeconds 300' '-quicktest' 'map test' } | Out-Null
Add-Heading $pHome 'Latest results'
$homeResults = Add-Note $pHome 'Loading...' 118
$homeResults.Tag = 'text'
Add-Action $pHome 'Refresh results' 'Re-read the latest summary and self-test (F5).' 'Reads reports\LATEST-SUMMARY.txt and reports\LATEST-SELFTEST.txt again and shows the key numbers above.' { Update-Home } | Out-Null

function Update-Home {
    $lines = @()
    # v13.76: one line about this PC's graphics (does it need the bridge at all?)
    if ($null -eq $script:gpuLine) {
        $script:gpuLine = ''
        try { . (Join-Path $script:root 'Gpu-Compat.ps1'); $script:gpuLine = Get-GpuCompatLine } catch { }
    }
    if ($script:gpuLine) { $lines += $script:gpuLine; $lines += '' }
    $sumFile = Join-Path $script:reports 'LATEST-SUMMARY.txt'
    if (Test-Path -LiteralPath $sumFile) {
        $t = @(Get-Content -LiteralPath $sumFile -ErrorAction SilentlyContinue)
        $when = (Get-Item -LiteralPath $sumFile).LastWriteTime.ToString('yyyy-MM-dd HH:mm')
        $kind = ''
        if ($t.Count -and $t[0] -match '\[([^\]]+)\]') { $kind = ', ' + $Matches[1] }
        $lines += "Last run ($when$kind):"
        # v13.55: prefer the gameplay figures (without loading and diagnostic-picture frames).
        $fs = $t | Where-Object { $_ -match 'Frame-time statistics \(gameplay\)' } | Select-Object -First 1
        $scope = 'while playing'
        if (-not $fs) { $fs = $t | Where-Object { $_ -match 'Frame-time statistics' } | Select-Object -First 1; $scope = 'whole session' }
        if ($fs -and $fs -match 'avg=([\d.,]+)fps 1%low=([\d.,]+)fps 0\.1%low=([\d.,]+)fps') {
            $lines += ('   {3}: average {0} fps   |   1% low {1} fps   |   0.1% low {2} fps   (the lows show stutter)' -f $Matches[1], $Matches[2], $Matches[3], $scope)
            if ($fs -match 'stutters\(>2\.5x median\)=(\d+) missed-refreshes=(\d+)') { $lines += ('   stutters: {0}   missed screen refreshes: {1}' -f $Matches[1], $Matches[2]) }
        } else {
            $fr = $t | Where-Object { $_ -match 'Frame pacing' } | Select-Object -First 1
            if ($fr -and $fr -match 'fps=([\d.,]+)') { $lines += ('   about {0} fps (older summary without 1%/0.1% lows)' -f $Matches[1]) } else { $lines += '   no frame-rate numbers in this summary' }
        }
    } else { $lines += 'No game run yet - start "Play RimWorld" or "Map test".' }
    $selfFile = Join-Path $script:reports 'LATEST-SELFTEST.txt'
    if (Test-Path -LiteralPath $selfFile) {
        $s = @(Get-Content -LiteralPath $selfFile -ErrorAction SilentlyContinue)
        $verdict = $s | Where-Object { $_ -match '^SELFTEST:' } | Select-Object -First 1
        $when = (Get-Item -LiteralPath $selfFile).LastWriteTime.ToString('yyyy-MM-dd HH:mm')
        $lines += ''
        $lines += ("Last self-test ($when): " + $(if ($verdict) { $verdict -replace '^SELFTEST:\s*', '' } else { 'see reports\LATEST-SELFTEST.txt' }))
    }
    $script:homeResults.Text = ($lines -join "`r`n")
}

# ---------- Play ----------
$pPlay = New-Page 'play' 'Play' 'Play' 'Normal play. Everything on the Options page applies. The game window may flicker briefly at start-up; that is normal.'
Add-Action $pPlay 'Play RimWorld' 'Main menu, your saves, normal speed.' 'Starts RimWorld through the bridge with diagnostics off. Use this for normal play. A run summary is written when the game closes.' { Start-Launcher 'Play RimWorld' '-QuickStart -SwapchainImages 1' '' 'play' } $true | Out-Null
Add-Action $pPlay 'Play: dev quicktest map' 'Starts straight into a small generated test map.' 'Skips the menu and opens a small developer test map (-quicktest). Good for quickly checking how the game looks and runs without loading a save.' { Start-Launcher 'Quicktest map' '-QuickStart -SwapchainImages 1' '-quicktest' 'quicktest play' } | Out-Null
Add-Action $pPlay 'Play with shader dump' 'Play, and save every translated shader.' 'Normal play, but the bridge writes every shader it translates into the run folder. Useful when a specific effect looks wrong; slightly slower start.' { Start-Launcher 'Shader dump' '-QuickStart -SwapchainImages 1 -DumpShaders' '' 'shader dump' } | Out-Null
Add-Note $pPlay 'Tip: open "Laptop health > Throttle monitor" before playing to see frames per second, CPU speed and temperature live.' 40 | Out-Null

# ---------- Tests ----------
$pTests = New-Page 'tests' 'Tests' 'Unattended tests' 'These start, run and close by themselves. Do not click in the game while they run, and look away from the screen (flicker is possible). Results land in the reports folder and on the Results page.'
Add-Action $pTests 'Map test (5 min)' 'Quicktest map with pictures + shaders; closes after 5 min.' 'Opens the quicktest map, saves frame pictures and translated shaders, and closes after 5 minutes. The summary shows frame rate, 1% and 0.1% lows, stutters and any rendering problems the bridge detected.' { Start-Launcher 'Map test' '-QuickStart -SwapchainImages 1 -FrameDiag -DumpShaders -AutoCloseSeconds 300' '-quicktest' 'map test' } $true | Out-Null
Add-Action $pTests 'Speed test (5 min, no pictures)' 'Like the map test, without frame pictures or shader dumps: true frame rate.' 'v13.70: the same unattended quicktest map, but without the diagnostics that save frame pictures every 20 seconds and inspect every frame (they cost 1.5-9 ms per frame and cause 2-8 s pauses). Use this to measure speed and stutter; use the map test to check pictures.' { Start-Launcher 'Speed test' '-QuickStart -SwapchainImages 1 -AutoCloseSeconds 300' '-quicktest' 'speed test' } $true | Out-Null
Add-Action $pTests 'Boot test' 'Starts, reaches the main menu, quits. Measures boot time.' 'Starts the game, waits for the main menu and quits by itself. Use it to measure start-up time or to check that a new version starts at all.' { Start-Launcher 'Boot test' '-QuickStart -SwapchainImages 1 -AutoCloseSeconds 240' '-QuitAfterBoot' 'boot only' } | Out-Null
Add-Action $pTests 'All tests in a row (~15 min)' 'Boot test + map tests with and without atlas compression.' 'Runs the boot test and two map tests (with and without atlas compression) one after another and writes a combined report. Leave the laptop alone for about 15 minutes.' { Start-Cmd 'Auto-Test-All.cmd' 'All tests' } | Out-Null
Add-Action $pTests 'Bridge self-test (1-2 min)' 'Developer test pictures on the HD 5500 and on WARP. No game.' 'Runs the bridge''s own test suite (about 28 small test pictures, each checking one feature that once caused a bug) on the real HD 5500 and on Microsoft''s software renderer WARP. RimWorld is not started. Lines ending in "-old" are proof runs with a fix switched off and are expected to show the old bug.' { Start-Cmd 'Laptop-Selftest.cmd' 'Self-test' } | Out-Null

# ---------- Diagnostics ----------
$pDiag = New-Page 'diag' 'Diagnostics' 'Diagnostics' 'Slower runs that record extra information, for finding the cause of a visual problem or crash.'
Add-Action $pDiag 'Full start-up checks + frame trace' 'Thorough launcher with all checks and frame pictures.' 'The slow, thorough launcher: all start-up checks plus frame pictures. Use it when the game does not start or shows something badly wrong.' { Start-Launcher 'Full diagnostics' '-FrameDiag -SwapchainImages 1' '' 'full diagnostics' } | Out-Null
Add-Action $pDiag 'Reference: real Vulkan on CPU' 'Mesa lavapipe instead of the bridge. 1-3 fps.' 'Runs the quicktest map on Mesa lavapipe (a correct Vulkan driver that runs on the CPU) instead of the bridge. Very slow, but shows what correct output looks like, so you can tell whether a glitch is the bridge''s fault or the game''s. Closes after 10 minutes.' { Start-Launcher 'Reference (lavapipe)' '-QuickStart -Reference -AutoCloseSeconds 600' '-quicktest' 'reference lavapipe' } | Out-Null
# v13.77: compare with another Vulkan driver the user downloaded (e.g. Mesa Dozen = Vulkan on D3D12). Nothing is installed.
Add-Action $pDiag 'Compare: other Vulkan driver' 'Pick a driver .json (e.g. Mesa Dozen). Experimental.' 'Runs the quicktest map on another Vulkan driver instead of the bridge, for example Mesa "Dozen" (Vulkan on Direct3D 12) from mesa-dist-win: pick its dzn_icd.x86_64.json. Nothing is installed or changed. Many old GPUs will fail here (Dozen needs more Direct3D 12 features than an HD 5500 offers); the report says why. Closes after 10 minutes.' {
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'Vulkan driver manifest (.json)'; $dlg.Filter = 'Vulkan driver manifest (*.json)|*.json'
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        # cmd /c + powershell -File: quote with double quotes (single quotes would stay in the value)
        if ($dlg.FileName -match '[&|<>^%"]') { $script:status.Text = 'That path contains special characters; move the driver to a simple folder (e.g. C:\mesa) and try again.'; return }
        Start-Launcher 'Other Vulkan driver' ('-QuickStart -AltIcd "' + $dlg.FileName + '" -AutoCloseSeconds 600') '-quicktest' 'alternative driver'
    }
} | Out-Null
Add-Action $pDiag 'Record Vulkan calls (GFXReconstruct)' 'Records the game''s Vulkan calls for replay. Large files.' 'Records every Vulkan call the game makes, so a problem can be replayed and studied in the emulator. Produces large files; only use when asked.' { Start-Launcher 'Record' '-QuickStart -SwapchainImages 1 -Capture' '' 'capture' } | Out-Null
Add-Action $pDiag 'Install diagnostic tools (optional)' 'RenderDoc, PresentMon, Intel GPA, HWiNFO and more via winget.' 'Installs optional analysis tools through winget: RenderDoc, PIX, Intel GPA, GPUView, PresentMon, HWiNFO, Vulkan SDK and more. Each is optional; see TOOLS.md for what each one is for.' { Start-Cmd 'Install-Tools.cmd' 'Install tools' } | Out-Null

# ---------- Laptop health ----------
$pHeat = New-Page 'heat' 'Laptop health' 'Laptop speed, heat and power' 'This laptop slows its processor down when it is hot or the power supply is limited ("throttling"). The monitor and PC check are read-only. The two Windows tweaks at the bottom change a setting only after you confirm, and each has an undo button.'
Add-Action $pHeat 'Throttle monitor (live window)' 'Live fps, CPU speed, firmware limit, temperature, battery.' 'Opens a small window that stays on top while you play: frames per second, real CPU speed, firmware limit, temperature (if the laptop exposes it), charger and battery, a 2-minute speed graph and a verdict (heat or power). Read-only.' { Start-Process powershell.exe -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',(Join-Path $script:root 'Throttle-Monitor.ps1')) -WorkingDirectory $script:root; $script:status.Text = 'Throttle monitor opened (stays on top; close it any time).' } $true | Out-Null
Add-Action $pHeat 'PC check (power, heat, disk, Windows)' 'Read-only report with suggestions, each with its undo.' 'v13.73: power plan, battery health and charger, RAM channels, graphics chips (some Z70-80 have a second GeForce chip), BIOS and Lenovo power software, firmware speed-limit events of the last week, free space and type of each drive, shader cache sizes, Defender status, Game Bar recording, visual effects, Turbo setting, busiest background programs - and a numbered list of suggestions at the end. Changes nothing. Saved in reports\power-check-...' { Start-Cmd 'Check-Power.cmd' 'PC check' } | Out-Null
$chkGuard = Add-Option $pHeat 'guard' 'Heat guard: cap to 30 fps while the laptop throttles or runs hot' 'Less work = less heat; lifts again after 20 s of normal speed.' 'When the CPU slows down (throttling, clock below 50%) or gets hot in 3 of the last 10 seconds, the bridge limits the game to 30 frames per second until it has been normal for 20 seconds (v13.71: also catches the short 2-second throttle cycles this laptop has). Only the game is affected. Off by default.'
$chkCap45 = Add-Option $pHeat 'fpscap45' 'Frame-rate cap 45 fps (less heat, always on)' 'Fewer frames per second = less CPU and GPU work = the laptop heats up later and throttles less often.' 'v13.71: the bridge limits the game to 45 frames per second (RW_GPU_FPS_CAP=45) and waits with a precise Windows timer, so the waiting costs no CPU. Game speed is not affected (RimWorld simulates up to 2x the speed multiplier ticks per frame). Combine with the heat guard: 45 fps normally, 30 fps while the laptop throttles. Off by default.'

Add-Heading $pHeat 'Windows tweaks (optional, each with undo)'
function Start-Tweak([string]$action, [string]$question) {
    $answer = [System.Windows.Forms.MessageBox]::Show($question, 'HD5500 GPU Bridge - Windows tweak', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { $script:status.Text = 'Nothing changed.'; return }
    $p = Start-Process powershell.exe -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', (Join-Path $script:root 'PC-Tweaks.ps1'), '-Action', $action) -WorkingDirectory $script:root -Wait -PassThru -WindowStyle Hidden
    $logFile = Join-Path $script:reports 'pc-tweaks-log.txt'
    $last = if (Test-Path -LiteralPath $logFile) { (Get-Content -LiteralPath $logFile -Tail 1) } else { '' }
    $script:status.Text = $(if ($p.ExitCode -eq 0) { $last } else { 'Failed: ' + $last })
}
Add-Action $pHeat 'Cool mode (Turbo off)' 'Copy of your power plan without Turbo; less heat, ~12% lower peak.' 'v13.73 experiment against the 22% throttle cycles: makes a COPY of your active power plan with "maximum processor state" 99%, which on this CPU means no Turbo (2.2 GHz base clock instead of up to 2.7 GHz), and activates it. Peak speed drops about 12%, heat drops a lot; on 15 W laptops this often keeps the clock steady instead of collapsing to 22%. Compare a 10-minute Speed test with and without. Your own plan is not modified. Undo with the next button.' { Start-Tweak 'CoolModeOn' "Make a copy of your current power plan with Turbo off (maximum processor state 99%) and switch to it?`r`n`r`nYour own plan is not changed. 'Undo cool mode' switches back and deletes the copy." } | Out-Null
Add-Action $pHeat 'Undo cool mode' 'Back to your previous power plan; the copy is deleted.' 'Re-activates the power plan that was active before "Cool mode" and deletes the copy. Logged in reports\pc-tweaks-log.txt.' { Start-Tweak 'CoolModeOff' 'Switch back to your previous power plan and delete the Cool mode copy?' } | Out-Null
Add-Action $pHeat 'Game Bar recording off' 'Stops Windows recording games in the background.' 'v13.73: switches off Xbox Game Bar captures and "Record in the background" for your Windows account (two registry values under HKCU\...\GameDVR). Background recording costs CPU and GPU on slow PCs. The previous values are saved; the next button restores them.' { Start-Tweak 'GameBarOff' 'Switch off Game Bar captures and background recording for your account? (Undo button restores the previous values.)' } | Out-Null
Add-Action $pHeat 'Game Bar recording: undo' 'Restores the Game Bar values from before.' 'Puts the two Game Bar values back exactly as they were before "Game Bar recording off".' { Start-Tweak 'GameBarOn' 'Restore the Game Bar settings as they were before?' } | Out-Null

# ---------- Results ----------
# v13.84: tick-box editor for local\local-env.txt (adds/removes the leading # of KEY=VALUE lines; nothing else changes).
function Show-LocalSwitches {
    $path = Join-Path $script:localDir 'local-env.txt'
    if (-not (Test-Path -LiteralPath $path)) { $script:status.Text = 'local\local-env.txt not found.'; return }
    $lines = [Collections.Generic.List[string]]([IO.File]::ReadAllLines($path))
    $items = New-Object Collections.Generic.List[object]
    $lastComment = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i]
        if ($t -match '^\s*(#?)((RW_LOCAL_|RW_OPT_|RW_GPU_|DOTNET_|WAYLITH_)[A-Za-z0-9_]+)=(.*)$') {
            $label = $Matches[2] + '=' + $Matches[4]
            if ($lastComment) { $label += '   - ' + $lastComment }
            $items.Add([pscustomobject]@{ Index = $i; On = ($Matches[1] -eq ''); Label = $label; Key = $Matches[2] })
            $lastComment = ''
        } elseif ($t -match '^\s*#\s*([A-J][0-9]+\.\s*.*)$') { $lastComment = $Matches[1].Trim() }
        elseif ($t -match '^\s*#\s{2,}') { }
        else { $lastComment = '' }
    }
    $f = New-Object System.Windows.Forms.Form
    $f.Text = 'Local build switches (ticked = on)'; $f.Width = 980; $f.Height = 640; $f.StartPosition = 'CenterParent'
    $list = New-Object System.Windows.Forms.CheckedListBox
    $list.Dock = 'Fill'; $list.CheckOnClick = $true; $list.HorizontalScrollbar = $true
    foreach ($it in $items) { [void]$list.Items.Add($it.Label, $it.On) }
    # v13.85: presets - one click ticks a ready-made test set (Save still required).
    $base = @('RW_LOCAL_GPU_PROFILE','RW_LOCAL_TICK_BUDGET_MS','RW_LOCAL_PERF_HUD','RW_LOCAL_PERF_CSV')
    $presets = [ordered]@{
        '1. Baseline: local defaults + HUD/CSV' = $base
        '2. + simulation caches (TPS)' = $base + @('RW_LOCAL_TPS_CACHES')
        '3. + lighter interface' = $base + @('RW_LOCAL_TEXT_CACHE','RW_LOCAL_ALERTS_MINI','RW_LOCAL_COLONISTBAR_ICONS','RW_LOCAL_GIZMO_CACHE','RW_LOCAL_PAWN_LABEL_ZOOM','RW_LOCAL_HIDE_UI')
        '4. + fewer effects' = $base + @('RW_LOCAL_FLECK_CULL','RW_LOCAL_FLECK_CAP','RW_LOCAL_MOTE_QUANT','RW_LOCAL_FALLCOLOR_CACHE','RW_LOCAL_WEATHER_MESH_FRAMES')
        '5. + extra simulation throttles' = $base + @('RW_LOCAL_PAWN_TICK_FLOOR','RW_LOCAL_FAILCHECK_THROTTLE','RW_LOCAL_ROOM_STATS_THROTTLE','RW_LOCAL_TURRET_IDLE','RW_LOCAL_PLAYLOG_CAP','RW_LOCAL_STEAL_VALUE_CACHE')
        '6. + faster loading' = $base + @('RW_LOCAL_LAZY_SECTIONS','RW_LOCAL_LOAD_FPS','WAYLITH_NO_ASSET_PREFETCH')
        '7. + Satori GC (experimental)' = $base + @('RW_LOCAL_SATORI_GC')
        '8. + zoom limit and no shadows (looks different)' = $base + @('RW_OPT_MAX_ZOOM','RW_LOCAL_NO_SHADOWS')
        '9. All safe tweaks together (2-6)' = $base + @('RW_LOCAL_TPS_CACHES','RW_LOCAL_TEXT_CACHE','RW_LOCAL_ALERTS_MINI','RW_LOCAL_COLONISTBAR_ICONS','RW_LOCAL_GIZMO_CACHE','RW_LOCAL_PAWN_LABEL_ZOOM','RW_LOCAL_HIDE_UI','RW_LOCAL_FLECK_CULL','RW_LOCAL_FLECK_CAP','RW_LOCAL_MOTE_QUANT','RW_LOCAL_FALLCOLOR_CACHE','RW_LOCAL_WEATHER_MESH_FRAMES','RW_LOCAL_PAWN_TICK_FLOOR','RW_LOCAL_FAILCHECK_THROTTLE','RW_LOCAL_ROOM_STATS_THROTTLE','RW_LOCAL_TURRET_IDLE','RW_LOCAL_PLAYLOG_CAP','RW_LOCAL_STEAL_VALUE_CACHE','RW_LOCAL_LAZY_SECTIONS','RW_LOCAL_LOAD_FPS','WAYLITH_NO_ASSET_PREFETCH')
        '0. Everything off' = @()
    }
    $top = New-Object System.Windows.Forms.FlowLayoutPanel; $top.Dock = 'Top'; $top.Height = 40
    $plabel = New-Object System.Windows.Forms.Label; $plabel.Text = 'Preset:'; $plabel.AutoSize = $true; $plabel.Padding = '0,8,0,0'
    $combo = New-Object System.Windows.Forms.ComboBox; $combo.DropDownStyle = 'DropDownList'; $combo.Width = 420
    foreach ($k in $presets.Keys) { [void]$combo.Items.Add($k) }
    $apply = New-Object System.Windows.Forms.Button; $apply.Text = 'Tick this preset'; $apply.Width = 150; $apply.Height = 28
    $apply.Add_Click({
        if ($combo.SelectedIndex -lt 0) { return }
        $want = $presets[$combo.SelectedItem]
        for ($k = 0; $k -lt $items.Count; $k++) {
            $key = $items[$k].Key
            $on = ($want -contains $key) -and -not ($key -eq 'RW_LOCAL_TPS_EXCLUDE')
            $list.SetItemChecked($k, $on)
        }
    })
    $top.Controls.AddRange(@($plabel, $combo, $apply))
    $bar = New-Object System.Windows.Forms.FlowLayoutPanel; $bar.Dock = 'Bottom'; $bar.Height = 44; $bar.FlowDirection = 'RightToLeft'
    $save = New-Object System.Windows.Forms.Button; $save.Text = 'Save'; $save.Width = 110; $save.Height = 32
    $cancel = New-Object System.Windows.Forms.Button; $cancel.Text = 'Cancel'; $cancel.Width = 110; $cancel.Height = 32
    $hint = New-Object System.Windows.Forms.Label; $hint.AutoSize = $true; $hint.Padding = '0,10,20,0'; $hint.Text = 'Pick a preset, press Tick this preset, then Save. Run presets 2-8 one at a time and compare with 1. Applies to the next game/test started from the Local build page.'
    $save.Add_Click({
        for ($k = 0; $k -lt $items.Count; $k++) {
            $idx = $items[$k].Index
            $body = $lines[$idx] -replace '^\s*#?\s*', ''
            $lines[$idx] = $(if ($list.GetItemChecked($k)) { $body } else { '#' + $body })
        }
        [IO.File]::WriteAllLines($path, $lines)
        $script:status.Text = 'Local switches saved (' + @($items | Where-Object { $true }).Count + ' listed).'
        $f.Close()
    })
    $cancel.Add_Click({ $f.Close() })
    $bar.Controls.AddRange(@($save, $cancel, $hint))
    $f.Controls.Add($list); $f.Controls.Add($top); $f.Controls.Add($bar)
    [void]$f.ShowDialog()
}
# v13.83: private local build (..\local next to this folder; never part of the public package). Page only shown when it exists.
$script:localDir = Join-Path (Split-Path -Parent $script:root) 'local'
if (Test-Path -LiteralPath (Join-Path $script:localDir 'local-env.txt')) {
    $ld = '"' + $script:localDir + '"'
    $pLocal = New-Page 'local' 'Local build' 'Private local build' 'Your own extra tweaks (local\local-env.txt + the private mod "HD5500 Local Tweaks"). Every switch is off until you remove the # in front of its line. Change one thing per speed test and compare with the normal Speed test. For normal play, enable "HD5500 Local Tweaks (private)" once in the game''s mod list; test runs load it automatically.'
    Add-Action $pLocal 'Play (local build)' 'Normal play with your local switches.' 'Starts RimWorld through the bridge with the switches from local\local-env.txt (and the private mod, if enabled in the mod list).' { Start-Launcher 'Play (local)' ('-QuickStart -SwapchainImages 1 -LocalDir ' + $ld) '' 'play local' } $true | Out-Null
    Add-Action $pLocal 'Speed test (local build, 5 min)' 'Unattended quicktest map with your local switches.' 'The same 5-minute speed test as on the Tests page, but with the local switches and the private mod loaded. Compare its summary with a normal Speed test.' { Start-Launcher 'Speed test (local)' ('-QuickStart -SwapchainImages 1 -AutoCloseSeconds 300 -LocalDir ' + $ld) '-quicktest' 'speed test local' } $true | Out-Null
    Add-Action $pLocal 'Quicktest map (local build)' 'Starts straight into the test map with your local switches.' 'Opens the developer quicktest map with the local switches; you play/look around yourself. Good for checking how a switch looks (HUD with F11 if RW_LOCAL_PERF_HUD=1).' { Start-Launcher 'Quicktest (local)' ('-QuickStart -SwapchainImages 1 -LocalDir ' + $ld) '-quicktest' 'quicktest local' } | Out-Null
    Add-Action $pLocal 'Switch tweaks on/off' 'Tick boxes for every switch in local-env.txt.' 'Opens a list of all local switches with a tick box each (ticked = on). Save writes local-env.txt for you (it only adds or removes the # in front of a line). Values such as 25 or 80 can be changed with Edit local switches.' { Show-LocalSwitches } $true | Out-Null
    Add-Action $pLocal 'Edit local switches' 'Opens local\local-env.txt in Notepad.' 'All local switches with an explanation per line. Remove the # in front of a line to switch it on, put it back to switch it off. Save, then start a game or test from this page.' { Open-Text (Join-Path $script:localDir 'local-env.txt') 'local\local-env.txt not found.' } | Out-Null
    Add-Action $pLocal 'Read: what each switch does' 'Opens local\README-LOCAL.md.' 'Background for every switch: where it comes from in the game code, what it changes and its side effects.' { Open-Text (Join-Path $script:localDir 'README-LOCAL.md') 'local\README-LOCAL.md not found.' } | Out-Null
    Add-Action $pLocal 'Open the performance CSV folder' 'Where RW_LOCAL_PERF_CSV writes one line per second.' 'Opens the reports folder (the default target of RW_LOCAL_PERF_CSV in local-env.txt). Each run with the CSV switch on writes perf-<date>.csv there.' { Start-Process explorer.exe (Join-Path $script:root 'reports') } | Out-Null
}

$pRes = New-Page 'results' 'Results' 'Results' 'Every run writes a folder in "reports" and a short summary. These buttons open the latest ones.'
Add-Action $pRes 'Latest run summary' 'Frame rate, 1%/0.1% lows, stutters, problems found.' 'Opens reports\LATEST-SUMMARY.txt: game and bridge errors, frame-time statistics (average, 1% low, 0.1% low, percentiles, stutters, missed refreshes), CPU speed and the bridge performance digest.' { Open-Text (Join-Path $script:reports 'LATEST-SUMMARY.txt') 'No summary yet - run a game or test first.' } $true | Out-Null
Add-Action $pRes 'Latest self-test' 'Pass/fail per test picture, both GPUs.' 'Opens reports\LATEST-SELFTEST.txt with one line per test picture for the HD 5500 and WARP. Lines ending in "-old" are proof runs and are expected to show the old bug.' { Open-Text (Join-Path $script:reports 'LATEST-SELFTEST.txt') 'No self-test yet.' } | Out-Null
Add-Action $pRes 'Frame times of the last run (CSV)' 'One line per frame - open in Excel/LibreOffice and chart it.' 'Opens frametimes.csv of the newest run folder: frame number, time, frame time, present time, bridge time and CPU speed for every frame. Chart the frametime_ms column to see stutter spikes.' {
    $last = Get-ChildItem -LiteralPath $script:reports -Directory -Filter 'gpu-run-*' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if ($last -and (Test-Path -LiteralPath (Join-Path $last.FullName 'frametimes.csv'))) { Start-Process (Join-Path $last.FullName 'frametimes.csv') } else { $script:status.Text = 'No frametimes.csv yet (needs v13.53 or newer and a finished run).' } } | Out-Null
Add-Action $pRes 'Clean up disk space' 'Keep the newest 3 runs, pictures as PNG, compress logs.' 'Deletes run folders older than the newest 3, keeps the newest self-test and power check, converts diagnostic pictures from BMP to PNG (lossless, 5-20x smaller) and turns on Windows NTFS compression for the text logs (transparent). Only touches the bridge''s own reports folder. Runs automatically after every game or test as well; a report is saved as reports\cleanup-<date>.txt.' { Start-Cmd 'Cleanup-Reports.cmd' 'Clean up disk space' } | Out-Null
Add-Action $pRes 'Open reports folder' 'All run folders, pictures and logs.' 'Opens the reports folder in Explorer: one folder per run (logs, pictures, frame times), self-test folders and power checks.' { New-Item -ItemType Directory -Force -Path $script:reports | Out-Null; Start-Process explorer.exe $script:reports } | Out-Null

# ---------- Options ----------
$pOpt = New-Page 'options' 'Options' 'Options' 'These apply to the next game or test you start from this window and are remembered between starts. The defaults are the recommended settings.'
Add-Heading $pOpt 'Compatibility'
$chkNoAtlas = Add-Option $pOpt 'noatlas' 'Atlas compression off (-noCompressAtlases)' 'Recommended. Skips the game''s GPU texture compressor for atlases.' 'Passes -noCompressAtlases to the game, so its GPU block-compression encoder is not used for texture atlases. Current workaround; recommended on.' $true
$chkReset = Add-Option $pOpt 'reset' 'Reset Vulkan fallback stage' 'Lets the game retry its normal Vulkan loader choice once.' 'Passes -ResetVulkanFallback so the game forgets an earlier fallback decision and retries its normal Vulkan loader choice.'
$chkWarp = Add-Option $pOpt 'warp' 'Use WARP software GPU (slow reference)' 'Runs on Microsoft''s software rasterizer instead of the HD 5500.' 'Runs the game on WARP, Microsoft''s software renderer, instead of the HD 5500. Very slow, but shows whether a glitch comes from the Intel driver or from the bridge.'
Add-Heading $pOpt 'Diagnostics'
$chkDebug = Add-Option $pOpt 'debug' 'D3D11 debug layer' 'Explains rejected Direct3D calls in the log (needs Windows "Graphics Tools").' 'Turns on Microsoft''s D3D11 debug layer if the optional Windows feature "Graphics Tools" is installed. Slower; its messages go to the run log as [RW-D3D11-DEBUG].'
$chkFrames = Add-Option $pOpt 'frames' 'Save frame pictures' 'Writes BMP snapshots every 20 s and at fixed frames into the run folder.' 'Saves pictures of what the bridge drew at fixed frames and every 20 seconds, plus detailed per-draw logs. Uses disk space; the map test does this anyway.'
$chkTicks = Add-Option $pOpt 'ticks' 'Tick-rate counter also during normal play' 'Measures game speed (ticks/s vs. target). Tests always use it. Your other mods are off for that run.' 'Loads the bridge''s small read-only tick-rate counter mod (mods\HD5500TickRate) also for Play. It measures how many game ticks per second RimWorld really simulates compared with the chosen speed (60 per second x speed). Test runs always use it. Because the game is started with -mod=, only the base game, DLCs and this counter are loaded for that run - your own mods are off. Off by default.'
$chkPriority = Add-Option $pOpt 'priority' 'Game before background programs (above-normal priority)' 'Windows gives RimWorld the CPU first; updaters/browsers get what is left. Only for the game, ends with it.' 'Runs the RimWorld process at "above normal" priority class (set by the bridge when the game starts, RW_GPU_PROCESS_PRIORITY=above). Background programs such as updaters, browsers and indexers then only get CPU time the game leaves over, which helps against short stutters on a 2-core CPU. Nothing in Windows is changed permanently; it ends when the game closes. "High" is deliberately not offered (it can make mouse and sound stutter). Off by default.'
$chkSubmit = Add-Option $pOpt 'submitthread' 'Bridge work on its own thread (experiment)' 'Overlaps the bridge with the game''s next frame. Laptop test: same average fps, better worst frames, less even.' 'The submit thread (like DXVK''s CS thread): the bridge translates and executes each frame on its own thread while the game already prepares the next one (RW_GPU_SUBMIT_THREAD=1). Laptop A/B test: same average fps, 99th-percentile frame 32 ms instead of 39 ms, 1% low +7%, but frame-to-frame evenness 67% instead of 77%. At most one frame of extra latency. Off by default.'
$chkShaders = Add-Option $pOpt 'shaders' 'Save translated shaders (HLSL)' 'Writes every translated shader into the run folder.' 'Writes the HLSL source of every shader the bridge translates into the run folder.'
Add-Heading $pOpt 'Windows'
$chkAutoClose = Add-Option $pOpt 'autoclose' 'Close test windows when finished' 'Console windows close 8 s after a run instead of waiting for a key.' 'When a test or game run ends, its black console window closes by itself after 8 seconds instead of waiting for a key press. Results stay in the reports folder.' $true
$optSummary = Add-Note $pOpt '' 40
function Update-OptionSummary {
    $on = @()
    foreach ($k in $script:optionBoxes.Keys) { if ($script:optionBoxes[$k].Checked) { $on += $script:optionBoxes[$k].Text } }
    $script:optSummary.Text = 'Active now: ' + $(if ($on.Count) { $on -join '  |  ' } else { 'none' })
}

# ---------- Help ----------
$pHelp = New-Page 'help' 'Help' 'Help' 'How to read the numbers and what to do when something looks wrong.'
Add-Note $pHelp ("Frame rate terms:`r`n" +
    "  average fps - frames per second over the whole run.`r`n" +
    "  1% low / 0.1% low - the frame rate of the slowest 1% / 0.1% of frames. Close to the average = smooth; far below = stutter.`r`n" +
    "  stutters - frames that took more than 2.5x the usual frame time.`r`n" +
    "  missed refreshes - times the screen showed the same picture again because a frame was late (60 Hz screen = 16.7 ms per refresh).") 100 | Out-Null
Add-Note $pHelp ("When something looks wrong:`r`n" +
    "  1. Take a screenshot and note the time.   2. Run a Map test (or keep playing) so the run summary is written.`r`n" +
    "  3. Compare with Diagnostics > Reference (lavapipe): if it looks the same there, it is the game, not the bridge.") 66 | Out-Null
Add-Action $pHelp 'Testing guide' 'What to check after each new version.' 'Opens TESTING.md: per version, what to look at in the game and in the summaries.' { Open-Text (Join-Path $script:root 'TESTING.md') 'TESTING.md not found.' } | Out-Null
Add-Action $pHelp 'Changelog' 'What changed in each version, and why.' 'Opens CHANGELOG-HD5500-BRIDGE.md: every version with the problem, the cause and the fix, explained.' { Open-Text (Join-Path $script:root 'CHANGELOG-HD5500-BRIDGE.md') 'Changelog not found.' } | Out-Null
Add-Action $pHelp 'Tools guide' 'Which optional analysis tool to use for what.' 'Opens TOOLS.md: what RenderDoc, PresentMon, Intel GPA, HWiNFO and the other optional tools are useful for.' { Open-Text (Join-Path $script:root 'TOOLS.md') 'TOOLS.md not found.' } | Out-Null

# ---------- navigation + theming ----------
$script:currentPage = 'home'
function Show-Page([string]$key) {
    foreach ($k in $script:pages.Keys) { $script:pages[$k].Visible = ($k -eq $key) }
    $script:currentPage = $key
    Apply-Theme
    if ($key -eq 'home') { Update-Home }
}

function Set-Colors($control, $t) {
    foreach ($c in $control.Controls) {
        $kind = $null
        if ($c.Tag -is [hashtable]) { $kind = $c.Tag.Kind } elseif ($c.Tag -is [string]) { $kind = $c.Tag }
        if ($c -is [System.Windows.Forms.Button]) {
            if ($script:navButtons.ContainsValue($c)) {
                $active = ($c.Tag -eq $script:currentPage)
                if ($active) { $c.BackColor = $t.Accent; $c.ForeColor = $t.AccentText; $c.FlatAppearance.MouseOverBackColor = $t.Accent }
                else { $c.BackColor = $t.Side; $c.ForeColor = $t.Text; $c.FlatAppearance.MouseOverBackColor = $t.CardHover }
                $c.FlatAppearance.BorderSize = 0
            } elseif ($kind -eq 'primary') {
                $c.BackColor = $t.Accent; $c.ForeColor = $t.AccentText; $c.FlatAppearance.BorderSize = 0; $c.FlatAppearance.MouseOverBackColor = [System.Windows.Forms.ControlPaint]::Light($t.Accent, 0.2)
            } else {
                $c.BackColor = $t.Card; $c.ForeColor = $t.Text; $c.FlatAppearance.BorderColor = $t.Border; $c.FlatAppearance.BorderSize = 1; $c.FlatAppearance.MouseOverBackColor = $t.CardHover
            }
        } elseif ($c -is [System.Windows.Forms.CheckBox]) {
            $c.ForeColor = $t.Text; $c.BackColor = $t.Back
        } elseif ($c -is [System.Windows.Forms.Label]) {
            $c.BackColor = [System.Drawing.Color]::Transparent
            if ($kind -eq 'muted') { $c.ForeColor = $t.Muted } elseif ($kind -eq 'accent') { $c.ForeColor = $t.Heading } else { $c.ForeColor = $t.Text }
        } elseif ($c -is [System.Windows.Forms.Panel]) {
            if ($c -eq $script:side -or $c -eq $script:navHost) { $c.BackColor = $t.Side } elseif ($c -eq $script:bottom) { $c.BackColor = $t.Card } else { $c.BackColor = $t.Back }
        }
        if ($c.Controls.Count) { Set-Colors $c $t }
    }
}
function Apply-Theme {
    $t = $script:themes[$script:settings.theme]
    $script:form.BackColor = $t.Back; $script:form.ForeColor = $t.Text
    Set-Colors $script:form $t
    $script:appVer.ForeColor = $t.Muted
    $script:status.ForeColor = $t.Warn
    $script:themeButton.Text = $(if ($script:settings.theme -eq 'dark') { 'Switch to light mode' } else { 'Switch to dark mode' })
    $script:themeButton.BackColor = $t.Card; $script:themeButton.ForeColor = $t.Text; $script:themeButton.FlatAppearance.BorderColor = $t.Border
    if ($script:form.IsHandleCreated) { Set-TitleBar $script:form ($script:settings.theme -eq 'dark') }
}
$themeButton.Add_Click({ $script:settings.theme = $(if ($script:settings.theme -eq 'dark') { 'light' } else { 'dark' }); Save-Settings; Apply-Theme; $w = $script:form.Width; $script:form.Width = $w + 1; $script:form.Width = $w; $script:form.Refresh() })
$themeButton.Add_MouseEnter({ Show-Detail 'Light / dark mode' 'Switches the colour scheme of this window. Your choice is remembered (Bridge-Menu.settings.json next to this script).' })

# Keyboard: Ctrl+1..8 jump to a page; F5 refreshes the Home results.
$form.Add_KeyDown({
    param($s, $e)
    if ($e.Control -and $e.KeyCode -ge [System.Windows.Forms.Keys]::D1 -and $e.KeyCode -le [System.Windows.Forms.Keys]::D8) {
        $index = [int]$e.KeyCode - [int][System.Windows.Forms.Keys]::D1
        $keys = @($script:pages.Keys)
        if ($index -lt $keys.Count) { Show-Page $keys[$index]; $e.Handled = $true }
    } elseif ($e.KeyCode -eq [System.Windows.Forms.Keys]::F5) { Show-Page 'home'; $e.Handled = $true }
})
$status.Text = 'Ctrl+1..8 switch pages, F5 refreshes results.  Photosensitivity: tests can flicker or flash - look away from unattended tests and stop if you feel unwell.'

# The dark title bar must be requested when the window handle is created (before it is first drawn);
# requesting it later only takes effect after a resize. v13.55.
$form.Add_HandleCreated({ Set-TitleBar $script:form ($script:settings.theme -eq 'dark') })
$form.Add_Shown({ Update-OptionSummary })
Show-Page 'home'
[void]$form.ShowDialog()
