#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$launcher = Join-Path (Split-Path -Parent $PSScriptRoot) 'Launch-HD5500-GPU.ps1'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($launcher,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Launcher parse failed.' }
$function = $ast.Find({param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Set-BridgeDiagnostics'},$true)
if (-not $function) { throw 'Diagnostic-mode helper missing.' }
Invoke-Expression $function.Extent.Text
$names = @('VK_LOADER_DEBUG','RW_HD5500_D3D_DIAGNOSTICS','RW_HD5500_D3D_DEBUG_LAYER','RW_HD5500_MEMORY_TRACE','RW_HD5500_BLIT_SCRATCH_TRACE','RW_HD5500_IMAGE_LIFETIME_TRACE','RW_HD5500_TEXTURE_TRACE','RW_HD5500_COMMAND_TRACE','RW_HD5500_BLIT_TRACE')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
function Require-Value([string]$Name,[string]$Expected) {
    $actual = [Environment]::GetEnvironmentVariable($Name,'Process')
    if ([string]$actual -ne $Expected) { throw ('Unexpected mode setting: ' + $Name + ' = ' + $actual) }
}
try {
    Set-BridgeDiagnostics $false
    Require-Value 'VK_LOADER_DEBUG' 'error,warn'
    Require-Value 'RW_HD5500_D3D_DEBUG_LAYER' '0'
    Require-Value 'RW_HD5500_D3D_DIAGNOSTICS' '1'
    Require-Value 'RW_HD5500_BLIT_SCRATCH_TRACE' '1'
    foreach ($name in @('RW_HD5500_TEXTURE_TRACE','RW_HD5500_COMMAND_TRACE','RW_HD5500_BLIT_TRACE')) { Require-Value $name '' }
    Set-BridgeDiagnostics $true
    Require-Value 'VK_LOADER_DEBUG' 'error,warn,driver,layer'
    Require-Value 'RW_HD5500_D3D_DEBUG_LAYER' '1'
    foreach ($name in $names | Where-Object { $_ -ne 'VK_LOADER_DEBUG' }) { Require-Value $name '1' }
    Set-BridgeDiagnostics $false
    Require-Value 'RW_HD5500_COMMAND_TRACE' ''
    Require-Value 'RW_HD5500_D3D_DEBUG_LAYER' '0'
    'PASS: normal mode is quiet, detailed mode is explicit, and mode switching clears old flags.'
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
foreach ($name in $names) {
    if ([string][Environment]::GetEnvironmentVariable($name,'Process') -ne [string]$saved[$name]) { throw 'Environment restoration failed.' }
}
'PASS: original process environment restored.'
