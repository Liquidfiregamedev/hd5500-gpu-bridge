# v13.73: optional, reversible Windows tweaks for this laptop, started only by a button the user clicks
# in the Bridge menu (Laptop health page), after a confirmation question there.
# Every action writes what it changed to reports\pc-tweaks-log.txt; every "Undo" puts it back.
#   -Action CoolModeOn   copy of the ACTIVE power plan with "maximum processor state" 99% (= no Turbo:
#                        the i5-5200U stays at its 2.2 GHz base clock instead of up to 2.7 GHz), activated.
#   -Action CoolModeOff  re-activates the plan that was active before and deletes the copy.
#   -Action GameBarOff   Game Bar background recording / captures off (current user only).
#   -Action GameBarOn    puts the two values back as they were before GameBarOff.
# No admin rights needed. Nothing else is changed. Defender exclusions are deliberately NOT offered
# here (security setting): the PC check prints the exact command for the user to decide.
param([Parameter(Mandatory = $true)][ValidateSet('CoolModeOn', 'CoolModeOff', 'GameBarOff', 'GameBarOn')][string]$Action)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$reports = Join-Path $root 'reports'
New-Item -ItemType Directory -Force -Path $reports | Out-Null
$state = Join-Path $reports 'pc-tweaks-state.json'
$log = Join-Path $reports 'pc-tweaks-log.txt'
function L([string]$s) { Add-Content -LiteralPath $log -Value ((Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '  ' + $s) -Encoding UTF8; Write-Host $s }
function Load { if (Test-Path -LiteralPath $state) { Get-Content -LiteralPath $state -Raw | ConvertFrom-Json } else { [pscustomobject]@{} } }
function Save($o) { $o | ConvertTo-Json | Set-Content -LiteralPath $state -Encoding UTF8 }
function Set-Prop($o, [string]$n, $v) { if ($o.PSObject.Properties[$n]) { $o.$n = $v } else { $o | Add-Member -NotePropertyName $n -NotePropertyValue $v } }
function Get-ActiveGuid { $t = (powercfg /getactivescheme) -join ' '; if ($t -match '([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})') { $Matches[1] } else { throw 'Active power plan not found' } }
$s = Load
try {
  switch ($Action) {
    'CoolModeOn' {
      if ($s.PSObject.Properties['coolPlan'] -and $s.coolPlan) { L ('Cool mode is already on (plan ' + $s.coolPlan + '). Use "Undo cool mode" first.'); break }
      $old = Get-ActiveGuid
      $dup = (powercfg /duplicatescheme $old) -join ' '
      if ($dup -notmatch '([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})') { throw ('powercfg /duplicatescheme failed: ' + $dup) }
      $new = $Matches[1]
      powercfg /changename $new 'HD5500 Cool mode (no Turbo)' 'Copy made by the HD5500 GPU Bridge menu: maximum processor state 99% = no Turbo. Undo in the menu.' | Out-Null
      powercfg /setacvalueindex $new SUB_PROCESSOR PROCTHROTTLEMAX 99 | Out-Null
      powercfg /setdcvalueindex $new SUB_PROCESSOR PROCTHROTTLEMAX 99 | Out-Null
      # the Bitsum plan keeps the minimum at 100%; a minimum above the maximum would be clamped anyway, set it explicitly
      powercfg /setacvalueindex $new SUB_PROCESSOR PROCTHROTTLEMIN 99 | Out-Null
      powercfg /setdcvalueindex $new SUB_PROCESSOR PROCTHROTTLEMIN 99 | Out-Null
      powercfg /setactive $new | Out-Null
      Set-Prop $s 'coolPlan' $new; Set-Prop $s 'previousPlan' $old; Save $s
      L ('Note: if Process Lasso is set to switch power plans by itself, it may switch back; then choose the plan in its tray menu.')
      L ('Cool mode ON: new plan ' + $new + ' (copy of ' + $old + ', maximum processor state 99% = no Turbo) is active. Undo: menu "Undo cool mode", or: powercfg /setactive ' + $old + ' ; powercfg /delete ' + $new)
    }
    'CoolModeOff' {
      if (-not ($s.PSObject.Properties['coolPlan'] -and $s.coolPlan)) { L 'Cool mode was not switched on by this menu; nothing to undo.'; break }
      powercfg /setactive $s.previousPlan | Out-Null
      powercfg /delete $s.coolPlan | Out-Null
      L ('Cool mode OFF: plan ' + $s.previousPlan + ' active again, copy ' + $s.coolPlan + ' deleted.')
      Set-Prop $s 'coolPlan' $null; Save $s
    }
    'GameBarOff' {
      $k = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GameDVR'
      New-Item -Path $k -Force | Out-Null
      $p = Get-ItemProperty -Path $k
      if (-not $s.PSObject.Properties['gameBarSaved']) {
        Set-Prop $s 'gameBarSaved' ([pscustomobject]@{ AppCaptureEnabled = $(if ($null -ne $p.AppCaptureEnabled) { [int]$p.AppCaptureEnabled } else { $null }); HistoricalCaptureEnabled = $(if ($null -ne $p.HistoricalCaptureEnabled) { [int]$p.HistoricalCaptureEnabled } else { $null }) })
        Save $s
      }
      Set-ItemProperty -Path $k -Name AppCaptureEnabled -Value 0 -Type DWord
      Set-ItemProperty -Path $k -Name HistoricalCaptureEnabled -Value 0 -Type DWord
      L 'Game Bar captures and background recording OFF (current user). Undo: menu "Game Bar recording: undo".'
    }
    'GameBarOn' {
      if (-not $s.PSObject.Properties['gameBarSaved']) { L 'Game Bar was not changed by this menu; nothing to undo.'; break }
      $k = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GameDVR'
      foreach ($n in 'AppCaptureEnabled', 'HistoricalCaptureEnabled') {
        $v = $s.gameBarSaved.$n
        if ($null -eq $v) { Remove-ItemProperty -Path $k -Name $n -ErrorAction SilentlyContinue } else { Set-ItemProperty -Path $k -Name $n -Value ([int]$v) -Type DWord }
      }
      $s.PSObject.Properties.Remove('gameBarSaved'); Save $s
      L 'Game Bar settings restored to what they were before.'
    }
  }
} catch { L ('FAILED: ' + $_.Exception.Message + ' (nothing else was changed)'); exit 1 }
