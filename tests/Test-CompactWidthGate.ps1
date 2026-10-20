param([string]$EvidenceDirectory = '')
$ErrorActionPreference = 'Stop'
$launcher = Join-Path (Split-Path -Parent $PSScriptRoot) 'Launch-HD5500-GPU.ps1'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($launcher,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Launcher parse errors.' }
$fn = $ast.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Confirm-CompactBacking' },$true)
if (-not $fn) { throw 'Compact-width function missing.' }
Invoke-Expression $fn.Extent.Text
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('RimWorldCompactGate-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    if ($EvidenceDirectory) {
        foreach ($mode in @('transfer','storage')) {
            $count = Confirm-CompactBacking (Join-Path $EvidenceDirectory ('compact-' + $mode + '-final.log'))
            if ($count -ne 9) { throw ('Unexpected compact binding count: ' + $count) }
            Write-Host ('PASS: actual ' + $mode + ' evidence contains nine compact 4 KiB bindings.')
        }
    }
    $valid = '[RW-GPU-MEMORY] stage=BUFFER_BIND_END parentBytes=16777216 bindingOffset=1048576 logicalBytes=4096 retainedParentObjects=1 nativeBufferWidth=4096 nativeLiveBufferBytes=4096'
    $sample = Join-Path $temporary 'native log with spaces.txt'
    @($valid) * 9 | Set-Content -LiteralPath $sample
    if ((Confirm-CompactBacking $sample) -ne 9) { throw 'Valid gate fixture rejected.' }
    Write-Host 'PASS: isolated gate positive control accepts nine compact records.'
    foreach ($invalid in @($valid.Replace('nativeBufferWidth=4096','nativeBufferWidth=16777216'),'')) {
        @($invalid) * 9 | Set-Content -LiteralPath $sample
        $refused = $false
        try { $null = Confirm-CompactBacking $sample } catch { $refused = $true }
        if (-not $refused) { throw 'Invalid or empty compact-width evidence accepted.' }
    }
    $refused = $false
    try { $null = Confirm-CompactBacking (Join-Path $temporary 'missing.txt') } catch { $refused = $true }
    if (-not $refused) { throw 'Missing compact-width log accepted.' }
    Write-Host 'PASS: parent-sized, empty and missing evidence rejected.'
    Write-Host 'Function/gate tests only; synthetic controls are not GPU validation.'
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
