$ErrorActionPreference='Stop'
$path=Join-Path (Split-Path -Parent $PSScriptRoot) 'Launch-HD5500-GPU.ps1'
$t=$null;$e=$null;$ast=[System.Management.Automation.Language.Parser]::ParseFile($path,[ref]$t,[ref]$e)
if($e.Count){throw ($e|Out-String)}
$fn=$ast.FindAll({param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Confirm-AtlasBlit'},$true)
if($fn.Count-ne 1){throw 'Atlas gate definition missing'}
Invoke-Expression $fn[0].Extent.Text
$dir=Join-Path ([IO.Path]::GetTempPath()) ('atlas gate '+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dir|Out-Null
function Expect-Failure([string]$name,[scriptblock]$call){$failed=$false;try{& $call|Out-Null}catch{$failed=$true};if(-not $failed){throw ('Gate accepted '+$name)};Write-Output ('PASS reject '+$name)}
try{
 $p=Join-Path $dir 'results with spaces.txt'
 Expect-Failure 'missing report' {Confirm-AtlasBlit $p}
 $good=('PIXEL PASS: exact'+[Environment]::NewLine+'CASE PASS: complete'+[Environment]::NewLine+'LIFETIME PASS: reset'+[Environment]::NewLine)*9+'FINAL: PASS status=0;'+[Environment]::NewLine
 [IO.File]::WriteAllText($p,$good)
 if((Confirm-AtlasBlit $p)-ne 9){throw 'Clean nine-case report rejected'}
 Write-Output 'PASS accept complete nine-case report'
 [IO.File]::WriteAllText($p,$good+'FAIL: exact readback mismatch'+[Environment]::NewLine)
 Expect-Failure 'pixel mismatch despite pass caption' {Confirm-AtlasBlit $p}
 [IO.File]::WriteAllText($p,'FINAL: PASS status=0;'+[Environment]::NewLine)
 Expect-Failure 'caption without cases' {Confirm-AtlasBlit $p}
 [IO.File]::WriteAllText($p,('PIXEL PASS: exact'+[Environment]::NewLine+'CASE PASS: complete'+[Environment]::NewLine+'LIFETIME PASS: reset'+[Environment]::NewLine)*8+'FINAL: PASS status=0;')
 Expect-Failure 'partial eight-case run' {Confirm-AtlasBlit $p}
 Write-Output 'FINAL: ATLAS GATE PASS'
}finally{Remove-Item -LiteralPath $dir -Recurse -Force}
