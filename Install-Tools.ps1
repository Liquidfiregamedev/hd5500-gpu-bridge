# v13.45: install the diagnostic tool set on this PC (optional, each tool is independent).
# Uses winget where a package exists; otherwise opens the official download page.
# Nothing here touches the game folder. Run from the Bridge menu or: powershell -ExecutionPolicy Bypass -File Install-Tools.ps1
# Usage: Install-Tools.ps1 [-Only renderdoc,pix,...] [-List]
param([string[]]$Only = @(), [switch]$List)
$ErrorActionPreference = 'Continue'
$tools = @(
  @{ Id='renderdoc';  Name='RenderDoc';                          Winget='BaldurKarlsson.RenderDoc';         Url='https://renderdoc.org/builds';                                   Sees='one frame: every Vulkan call the game makes and every D3D11 call the bridge makes, textures, shaders' }
  @{ Id='pix';        Name='PIX for Windows';                    Winget='Microsoft.PIX';                    Url='https://devblogs.microsoft.com/pix/download/';                    Sees='D3D11 frame capture with GPU timings and per-pixel shader debugging (overlaps RenderDoc)' }
  @{ Id='gpa';        Name='Intel Graphics Performance Analyzers'; Winget='Intel.GraphicsPerformanceAnalyzers'; Url='https://www.intel.com/content/www/us/en/developer/tools/graphics-performance-analyzers/overview.html'; Sees='the HD 5500 itself: per-draw GPU time, shader busy, bandwidth' }
  @{ Id='wpt';        Name='Windows Performance Toolkit (GPUView, WPA)'; Winget='Microsoft.WindowsADK';    Url='https://learn.microsoft.com/windows-hardware/get-started/adk-install'; Sees='CPU vs GPU vs driver timeline (ETW): what a freeze waits on' }
  @{ Id='presentmon'; Name='PresentMon';                         Winget='Intel.PresentMon';                 Url='https://github.com/GameTechDev/PresentMon/releases';             Sees='frame-time log of every Present(): objective stutter measurement' }
  @{ Id='hwinfo';     Name='HWiNFO';                             Winget='REALiX.HWiNFO';                    Url='https://www.hwinfo.com/download/';                               Sees='GPU/CPU clocks and temperatures over time: thermal throttling during tests' }
  @{ Id='vulkansdk';  Name='Vulkan SDK (vkconfig, validation layers, GFXReconstruct, Hardware Capability Viewer)'; Winget='KhronosGroup.VulkanSDK'; Url='https://vulkan.lunarg.com/sdk/home#windows'; Sees='record/replay the game''s Vulkan stream, validation of the game''s own calls, what our ICD advertises' }
  @{ Id='dependencies'; Name='Dependencies (lucasg)';            Winget='lucasg.Dependencies';              Url='https://github.com/lucasg/Dependencies/releases';                Sees='which DLLs load and from where (loader stage, bridge DLL resolution)' }
  @{ Id='procmon';    Name='Process Monitor (Sysinternals)';     Winget='Microsoft.Sysinternals.ProcessMonitor'; Url='https://learn.microsoft.com/sysinternals/downloads/procmon'; Sees='file and registry activity during the map-load freeze' }
  @{ Id='procexp';    Name='Process Explorer (Sysinternals)';   Winget='Microsoft.Sysinternals.ProcessExplorer'; Url='https://learn.microsoft.com/sysinternals/downloads/process-explorer'; Sees='per-process GPU memory (shared memory on the HD 5500), handles, loaded DLLs' }
  @{ Id='dnspy';      Name='dnSpyEx';                            Winget='dnSpyEx.dnSpy';                    Url='https://github.com/dnSpyEx/dnSpy/releases';                      Sees='live debugging of the game''s .NET code (breakpoints in Waylith)' }
  @{ Id='dotnet-trace'; Name='dotnet-trace / dotnet-counters / dotnet-dump'; Winget=$null; Url='https://learn.microsoft.com/dotnet/core/diagnostics/dotnet-trace'; Sees='.NET side of a freeze: garbage collection pauses, JIT, lock waits'; Cmd='dotnet tool install -g dotnet-trace; dotnet tool install -g dotnet-counters; dotnet tool install -g dotnet-dump' }
  @{ Id='apitrace';   Name='apitrace';                           Winget=$null;                              Url='https://github.com/apitrace/apitrace/releases';                  Sees='records D3D11 calls the bridge emits (second opinion to RenderDoc)' }
)
if ($List) { $tools | ForEach-Object { Write-Host ('{0,-13} {1}' -f $_.Id, $_.Name); Write-Host ('              sees: ' + $_.Sees) }; return }
$have = Get-Command winget -ErrorAction SilentlyContinue
foreach ($t in $tools) {
  if ($Only.Count -gt 0 -and $Only -notcontains $t.Id) { continue }
  Write-Host ''; Write-Host ('== ' + $t.Name) -ForegroundColor Cyan
  Write-Host ('   sees: ' + $t.Sees)
  $ok = $false
  if ($t.Cmd) { try { Invoke-Expression $t.Cmd; $ok = $true } catch { Write-Host ('   failed: ' + $_) } }
  elseif ($t.Winget -and $have) {
    $p = Start-Process winget -ArgumentList @('install','--id',$t.Winget,'-e','--accept-package-agreements','--accept-source-agreements','--silent') -Wait -PassThru -NoNewWindow
    $ok = ($p.ExitCode -eq 0 -or $p.ExitCode -eq -1978335189)   # 0 = installed, -1978335189 = already installed
    if (-not $ok) { Write-Host ('   winget exit code ' + $p.ExitCode) }
  }
  if (-not $ok) { Write-Host ('   opening download page: ' + $t.Url); Start-Process $t.Url }
}
Write-Host ''; Write-Host 'Done. See TOOLS.md for what each tool is used for and how.'
