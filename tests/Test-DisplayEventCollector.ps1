$ErrorActionPreference = 'Stop'
$launcher = Join-Path (Split-Path -Parent $PSScriptRoot) 'Launch-HD5500-GPU.ps1'
$tokens=$null; $errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile($launcher,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Launcher parser failed.'}
$functionAst=$ast.Find({param($a) $a -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $a.Name -eq 'Save-GraphicsResetEvents'},$true)
if(-not $functionAst){throw 'Event collector function not found.'}
Invoke-Expression $functionAst.Extent.Text
$script:FailMock=$false
$script:SeenFilter=$null
function Get-WinEvent {
    [CmdletBinding()]
    param([hashtable]$FilterHashtable,[int]$MaxEvents)
    $script:SeenFilter=$FilterHashtable
    if($MaxEvents -ne 20){throw 'Event count is not bounded at 20.'}
    if($script:FailMock){throw 'Fixture: matching events unavailable.'}
    [pscustomobject]@{TimeCreated=[DateTime]::Now;Id=4101;ProviderName='Display';LevelDisplayName='Warning';Message='Fixture display event, not a real Windows reset.'}
}
$directory=Join-Path ([IO.Path]::GetTempPath()) ('RimWorld display fixture ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory | Out-Null
try {
    $since=[DateTime]::UtcNow
    Save-GraphicsResetEvents $directory $since
    $text=Get-Content -LiteralPath (Join-Path $directory 'Recent-Display-Events.txt') -Raw
    if($text -notmatch 'Fixture display event'){throw 'Mock event was not written.'}
    if($script:SeenFilter.LogName -ne 'System' -or $script:SeenFilter.ProviderName -ne 'Display'){throw 'Event query exceeded intended provider scope.'}
    if($script:SeenFilter.StartTime -ne $since.AddSeconds(-30).ToLocalTime()){throw 'Event start time differed from intended launch window.'}
    Write-Host 'PASS: bounded Display-only query, launch-window time, and spaced output path.'
    $script:FailMock=$true
    Save-GraphicsResetEvents $directory $since
    $text=Get-Content -LiteralPath (Join-Path $directory 'Recent-Display-Events.txt') -Raw
    if($text -notmatch 'Absence of this event does not exclude graphics-device loss'){throw 'Unavailable-event note is incomplete.'}
    Write-Host 'PASS: unavailable event read is recorded without failing the launcher.'
    Write-Host 'Mocked event-source test only; no real Windows events or settings accessed.'
} finally { Remove-Item -LiteralPath $directory -Recurse -Force }
