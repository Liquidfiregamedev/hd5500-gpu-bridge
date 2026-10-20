#requires -Version 5.1
param([string]$Launcher = (Join-Path $PSScriptRoot '..\Launch-HD5500-GPU.ps1'))
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Launcher,[ref]$tokens,[ref]$errors)
if ($errors.Count) { $errors | Format-List; throw 'Launcher syntax failed.' }
$fn = @($ast.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Stage-Temporary'},$true))
if ($fn.Count -ne 1) { throw 'Could not identify exactly one staging function.' }
. ([ScriptBlock]::Create($fn[0].Extent.Text))
$temporaryFiles = @{}
$root = Join-Path ([IO.Path]::GetTempPath()) ('RimWorldLauncherSafety-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $source = Join-Path $root 'test source.dll'
    $new = Join-Path $root 'temporary destination.dll'
    [IO.File]::WriteAllText($source,'test bytes; not a DLL')
    Stage-Temporary $source $new
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $new).Hash -or -not $temporaryFiles.ContainsKey($new)) { throw 'New-file staging/tracking failed.' }
    Write-Host 'PASS: new temporary file copied and tracked by exact content hash.'
    $same = Join-Path $root 'identical existing.dll'
    Copy-Item -LiteralPath $source -Destination $same
    Stage-Temporary $source $same
    if ($temporaryFiles.ContainsKey($same)) { throw 'Existing identical file was incorrectly marked for removal.' }
    Write-Host 'PASS: identical pre-existing file reused without marking it for deletion.'
    $unrelated = Join-Path $root 'unrelated existing.dll'
    [IO.File]::WriteAllText($unrelated,'different user-owned bytes')
    $before = (Get-FileHash -LiteralPath $unrelated).Hash
    $refused = $false
    try { Stage-Temporary $source $unrelated } catch { $refused = $true }
    if (-not $refused -or (Get-FileHash -LiteralPath $unrelated).Hash -ne $before) { throw 'Unknown-file protection failed.' }
    Write-Host 'PASS: unrelated existing file refused and preserved unchanged.'
    $directory = Join-Path $root 'existing directory'
    New-Item -ItemType Directory -Path $directory | Out-Null
    $refused = $false
    try { Stage-Temporary $source $directory } catch { $refused = $true }
    if (-not $refused -or -not (Test-Path -LiteralPath $directory -PathType Container)) { throw 'Directory protection failed.' }
    Write-Host 'PASS: directory destination refused without alteration.'
    Write-Host 'PASS: syntax and staging unit tests complete; no game, driver or Windows file was used.'
} finally {
    # Remove only this test's freshly created random directory, never game/user data.
    Remove-Item -LiteralPath $root -Recurse -Force
}
