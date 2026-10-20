#requires -Version 5.1
param([string]$EvidenceDirectory = (Join-Path $PSScriptRoot 'evidence'))
$ErrorActionPreference = 'Stop'
$launcher = Join-Path (Split-Path -Parent $PSScriptRoot) 'Launch-HD5500-GPU.ps1'
$t=$null;$e=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile($launcher,[ref]$t,[ref]$e)
if($e.Count){throw 'Launcher parse failed.'}
foreach($name in @('Confirm-AtlasBatch','Confirm-ImageLifetime')) {
    $f=$ast.Find({param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
    if(-not $f){throw ('Missing function '+$name)}
    Invoke-Expression $f.Extent.Text
}
$batch=Join-Path $EvidenceDirectory 'new-batch-mip.log'
$image=Join-Path $EvidenceDirectory 'new-image-lifetime.log'
$null=Confirm-AtlasBatch $batch $batch
$null=Confirm-ImageLifetime $image $image
'PASS: real exact-pixel batch and image-lifetime evidence accepted.'
$temp=Join-Path ([IO.Path]::GetTempPath()) ('V11GateTest-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
function Must-Reject([scriptblock]$Test,[string]$Label) {
    $rejected=$false
    try { & $Test | Out-Null } catch { $rejected=$true }
    if(-not $rejected){throw ('Incorrectly accepted '+$Label)}
    'PASS reject: '+$Label
}
try {
    $bad=Join-Path $temp 'bad.log'
    $text=[IO.File]::ReadAllText($batch)
    [IO.File]::WriteAllText($bad,$text.Replace('ATLAS_SECOND_FULL_PIXEL_CHECK: PASS','ATLAS_SECOND_FULL_PIXEL_CHECK: MISSING'))
    Must-Reject { Confirm-AtlasBatch $bad $bad } 'incomplete repeated pixels'
    [IO.File]::WriteAllText($bad,([regex]::Replace($text,'peakBytes=\d+','peakBytes=1073741824')))
    Must-Reject { Confirm-AtlasBatch $bad $bad } 'excessive compact texel payload'
    [IO.File]::WriteAllText($bad,$text.Replace('event=release','event=not-release'))
    Must-Reject { Confirm-AtlasBatch $bad $bad } 'missing releases'
    [IO.File]::WriteAllText($bad,$text+[Environment]::NewLine+'[RW-GPU-DEVICE-FAIL] forced negative control')
    Must-Reject { Confirm-AtlasBatch $bad $bad } 'native device failure despite pass caption'
    [IO.File]::WriteAllText($bad,'FINAL: ATLAS BATCH PASS')
    Must-Reject { Confirm-AtlasBatch $bad $bad } 'pass caption without data and accounting'
    Must-Reject { Confirm-AtlasBatch (Join-Path $temp 'missing') $batch } 'missing output file'
    [IO.File]::WriteAllText($bad,([IO.File]::ReadAllText($image).Replace('IMAGE_RESOURCE_RELEASE','RELEASE_MISSING')))
    Must-Reject { Confirm-ImageLifetime $bad $bad } 'missing image native-release evidence'
} finally { Remove-Item -LiteralPath $temp -Recurse -Force }
'FINAL: V11 GATES PASS. Negative controls are guard tests, not hardware validation.'
