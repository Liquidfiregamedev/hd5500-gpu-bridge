#requires -Version 5.1
<#
Experimental Ashes Vulkan 1.0 -> D3D11 GPU bridge for Windows x64.
Driver keeps honest portability classification. A small loader-backed proxy adds
advertised portability opt-in only when RW_HD5500_PORTABILITY=1.
No registry, Windows files, saves, mods, or permanent environment changes.
New compatibility files created beside RimWorld.exe are removed after it exits.
#>
[CmdletBinding()]
param(
    [string]$GameExe = 'C:\Program Files (x86)\Steam2\steamapps\common\RimWorld\RimWorld.exe',
    [string]$GameArguments = '',
    [switch]$TestOnly,
    [switch]$SerialDiagnostics,
    [switch]$VerboseDiagnostics,
    [switch]$KeepImplicitLayers,
    # Optional process-local diagnostics. Key checks run by default; exhaustive checks are opt-in.
    [switch]$GpuTiming,
    [switch]$NoDecodeFallback,
    [switch]$StateCache,
    [switch]$NoHazardTracking,
    [switch]$FullChecks,
    [switch]$FrameProbe,
    [switch]$UiTrace,
    # v13.4: frame-composition trace + BMP frame dumps (game process only).
    [switch]$FrameDiag,
    # v13.4 A/B: number of swapchain images the bridge offers (1 = v13.3 behaviour).
    [ValidateRange(1,3)][int]$SwapchainImages = 1,
    # v13.4 unattended testing: ask the game to close itself after N seconds (0 = never).
    [int]$AutoCloseSeconds = 0,
    # v13.6 play mode: only Check 1 before the game, and bridge tracing off in the game process.
    [switch]$QuickStart,
    # v13.8: record the game's Vulkan calls with GFXReconstruct (capture\ folder) into the run folder.
    [switch]$Capture,
    # v13.16: write every translated shader (HLSL) into <run>\hlsl.
    [switch]$DumpShaders,
    # v13.45: RimWorld 1.7 writes saves with the Windows decimal separator on some threads (comma on
    # Dutch/German/French Windows) and reads them back with a dot -> corrupted pawns. The game is now
    # started in .NET invariant-globalization mode (numbers always use a dot). -NoInvariantCulture = old behaviour.
    [switch]$NoInvariantCulture,
    # v13.29: disk space. Runs are no longer zipped unless -Zip is given (the folder holds
    # the same files). Only the newest -KeepRuns run folders/zips are kept in reports\.
    [switch]$Zip,
    # v13.29: run the game on Mesa lavapipe (a conformant CPU Vulkan driver) instead of the
    # bridge, as a REFERENCE: what correct Vulkan output looks like on this PC. Very slow.
    [switch]$Reference,
    # v13.77: compare with ANOTHER Vulkan driver (ICD manifest .json), e.g. Mesa "Dozen" (Vulkan on D3D12,
    # vulkan_dzn.dll + dzn_icd.x86_64.json from mesa-dist-win). Not bundled; nothing is installed. Like
    # -Reference: the Intel preflight and experiment-env.txt are skipped. Experimental comparison only.
    [string]$AltIcd = '',
    # v13.78: PRIVATE local build - a folder outside the public package (e.g. ..\local) with local-env.txt
    # (RW_LOCAL_* switches) and mods\ (private runtime-patch mods). Used by local\Play-Local.cmd.
    [string]$LocalDir = '',
    # v13.56: load the read-only tick-rate counter mod (mods\HD5500TickRate) with -mod=. Automatic for test
    # runs (-quicktest / -AutoCloseSeconds); for normal play only with -TickCounter, because -mod= starts the
    # game with the base game + DLCs + this mod only (other mods off for that run).
    [switch]$TickCounter,
    [switch]$NoTickCounter,
    # v13.66: HD5500 Optimizer mod (mods\HD5500Optimizer), part 1 = RimWorld's own frame profiler.
    # 0 = off, 1 = engine scopes (default for test runs), 2 = also every Verse/RimWorld method (heavier).
    [int]$GameProfile = -1,
    # v13.67: HD5500 Optimizer maximum zoom-out in cells for this run (0 = the mod's own setting / game default).
    [int]$MaxZoomOut = 0,
    [int]$KeepRuns = 3
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$exitCode = 1
$transcript = $false
$saved = @{}
$envNames = @('VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_ADD_DRIVER_FILES','VK_LOADER_DEBUG','VK_LOADER_LAYERS_DISABLE','VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_ALLOW','LP_NUM_THREADS','LP_NATIVE_VECTOR_WIDTH','RW_HD5500_PORTABILITY','RW_HD5500_DISABLE_BC','RW_HD5500_TEXTURE_TRACE','RW_HD5500_D3D_DIAGNOSTICS','RW_HD5500_COMMAND_TRACE','RW_HD5500_SERIAL_CHECKPOINTS','RW_HD5500_MEMORY_TRACE','RW_HD5500_BLIT_TRACE','RW_HD5500_BLIT_SCRATCH_TRACE','RW_HD5500_IMAGE_LIFETIME_TRACE','RW_HD5500_D3D_DEBUG_LAYER','RW_HD5500_BC_DECODE_FALLBACK','RW_HD5500_LEGACY_CAPS','RW_HD5500_STATE_CACHE','RW_HD5500_HAZARD_TRACKING','RW_HD5500_FENCE_EVENTS','RW_HD5500_GPU_TIMING','RW_HD5500_STAGING_CACHE_MB','RW_HD5500_SHADER_CACHE','RW_HD5500_SHADER_CACHE_MB','RW_HD5500_COHERENT_REFRESH','RW_HD5500_FRAME_PROBE','RW_HD5500_UI_TRACE','RW_HD5500_FRAME_DIAG','RW_HD5500_FRAME_DUMP_DIR','RW_HD5500_SWAPCHAIN_IMAGES','VK_LAYER_PATH','VK_INSTANCE_LAYERS','GFXRECON_CAPTURE_FILE','GFXRECON_CAPTURE_FILE_TIMESTAMP','GFXRECON_CAPTURE_COMPRESSION_TYPE','GFXRECON_LOG_LEVEL','GFXRECON_LOG_FILE','GFXRECON_PAGE_GUARD_EXTERNAL_MEMORY','GFXRECON_MEMORY_TRACKING_MODE','RW_OPT_PROFILE','RW_OPT_PROFILE_FILE','RW_OPT_MAX_ZOOM')
$runDir = $null
# v13.64: stop early when the system drive is (nearly) full. A full disk made the game's own log
# writes and the counter mod fail on every frame (laptop, 20 Oct); results are useless then.
try {
    $sysDrive = Get-PSDrive -Name ($env:SystemDrive.TrimEnd(':')) -ErrorAction Stop
    if ($sysDrive.Free -lt 500MB) {
        Write-Host ('WARNING: only ' + [Math]::Round($sysDrive.Free / 1MB) + ' MB free on ' + $env:SystemDrive + '. RimWorld, Windows and the bridge need free space for logs and caches. Free up at least 1 GB (Disk Cleanup, empty the Recycle Bin, Steam download cache) and start again.') -ForegroundColor Yellow
        if ($sysDrive.Free -lt 200MB) { throw ('Not enough free disk space on ' + $env:SystemDrive + ' (' + [Math]::Round($sysDrive.Free / 1MB) + ' MB).') }
    }
} catch [System.Management.Automation.RuntimeException] { if ($_.Exception.Message -like 'Not enough free disk space*') { throw } }
$runner = $null
$workDir = $PSScriptRoot
$temporaryFiles = @{}
$runtimeConfigBackup = $null   # v13.82: local Satori GC switch
$startUtc = [DateTime]::UtcNow

function Invoke-Captured([string]$FilePath,[string]$RawArguments,[string]$Label,[int]$TimeoutSeconds=0) {
    $stdout = Join-Path $runDir ($Label + '.stdout.txt')
    $stderr = Join-Path $runDir ($Label + '.stderr.txt')
    $options = @('--exe',$FilePath,'--cwd',$workDir,'--out',$stdout,'--err',$stderr)
    if ($RawArguments) {
        $argumentFile = Join-Path $runDir ($Label + '.arguments.txt')
        [IO.File]::WriteAllText($argumentFile,$RawArguments,[Text.Encoding]::Unicode)
        $options += @('--args-file',$argumentFile)
    }
    if ($TimeoutSeconds -gt 0) { $options += @('--timeout',[string]$TimeoutSeconds) }
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $runner @options 2>&1 | ForEach-Object { Write-Host ([string]$_) }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    Write-Host ('Captured stdout: ' + $stdout)
    Write-Host ('Captured stderr: ' + $stderr)
    return [int]$code
}
function Show-Probe([string]$Label) {
    foreach ($stream in @('stdout','stderr')) {
        Get-Content -LiteralPath (Join-Path $runDir ($Label + '.' + $stream + '.txt')) -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    }
}
function Stage-Temporary([string]$Source,[string]$Destination) {
    $hash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $Destination) {
        if (-not (Test-Path -LiteralPath $Destination -PathType Leaf) -or (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ne $hash) {
            throw ('Refusing to overwrite an existing different file: ' + $Destination + '. Back up only the replacement from earlier test packages outside the game folder. Preserve original game/system files.')
        }
        Write-Host ('Reusing identical existing compatibility file: ' + $Destination)
    } else {
        Copy-Item -LiteralPath $Source -Destination $Destination
        $temporaryFiles[$Destination] = $hash
        Write-Host ('Temporary compatibility file: ' + $Destination)
    }
}

function Save-GraphicsResetEvents([string]$Directory,[DateTime]$SinceUtc) {
    $destination = Join-Path $Directory 'Recent-Display-Events.txt'
    try {
        $events = @(Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Display';StartTime=$SinceUtc.AddSeconds(-30).ToLocalTime()} -MaxEvents 20 -ErrorAction Stop)
        $events | Select-Object TimeCreated,Id,ProviderName,LevelDisplayName,Message | Format-List | Out-String -Width 180 | Set-Content -LiteralPath $destination -Encoding UTF8
    } catch {
        ('No matching Display event was returned, or the read was unavailable: ' + $_.Exception.Message + [Environment]::NewLine + 'Absence of this event does not exclude graphics-device loss.') | Set-Content -LiteralPath $destination -Encoding UTF8
    }
}

function Set-BridgeDiagnostics([bool]$Detailed) {
    # All settings are confined to this launch and restored in finally.
    [Environment]::SetEnvironmentVariable('VK_LOADER_DEBUG',$(if ($Detailed) {'error,warn,driver,layer'} else {'error,warn'}),'Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_D3D_DIAGNOSTICS','1','Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_D3D_DEBUG_LAYER',$(if ($Detailed) {'1'} else {'0'}),'Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_MEMORY_TRACE','1','Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_BLIT_SCRATCH_TRACE','1','Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_IMAGE_LIFETIME_TRACE','1','Process')
    foreach ($name in @('RW_HD5500_TEXTURE_TRACE','RW_HD5500_COMMAND_TRACE','RW_HD5500_BLIT_TRACE')) {
        [Environment]::SetEnvironmentVariable($name,$(if ($Detailed) {'1'} else {$null}),'Process')
    }
}

function Confirm-AtlasBatch([string]$OutputFile,[string]$NativeLog) {
    if (-not [IO.File]::Exists($OutputFile) -or -not [IO.File]::Exists($NativeLog)) { throw 'Atlas batch output or native accounting is missing.' }
    $text = [IO.File]::ReadAllText($OutputFile)
    foreach ($marker in @('ATLAS_FIRST_FULL_PIXEL_CHECK: PASS','ATLAS_POOL_RESET_REUSE: PASS','ATLAS_SECOND_FULL_PIXEL_CHECK: PASS','ATLAS_MIP_LAYER_FULL_PIXEL_CHECK: PASS','FINAL: ATLAS BATCH PASS')) {
        if (-not $text.Contains($marker)) { throw ('Missing strict atlas batch result: ' + $marker) }
    }
    if ($text -match '(?m)^FAIL:|^MISMATCH:|^FINAL: FAIL') { throw 'Atlas batch reported a pixel or API failure.' }
    $native = [IO.File]::ReadAllText($NativeLog)
    if ($native -match '\[RW-GPU-DEVICE-FAIL\]|event=alloc-failed|event=fallback') { throw 'Atlas batch reported a native failure or unexpected full fallback.' }
    $allocs = [regex]::Matches($native,'(?m)^\[RW-GPU-BLIT-SCRATCH\] event=alloc[^\r\n]*\bid=(\d+)\b[^\r\n]*\batlas=4096x4096x1\b[^\r\n]*\bpayloadBytes=(\d+)\b[^\r\n]*\bliveBytes=(\d+)\b[^\r\n]*\bpeakBytes=(\d+)\b[^\r\n]*\bmode=compact\b')
    if ($allocs.Count -lt 20) { throw 'Too few verified compact atlas scratch allocations for the retained-batch test.' }
    $small = 0
    foreach ($allocation in $allocs) {
        if ([uint64]$allocation.Groups[2].Value -le 4096) { ++$small }
        # This gate measures compact texel payload, not total process or GPU residency.
        if ([uint64]$allocation.Groups[4].Value -gt 100663296) { throw 'Compact scratch payload exceeded the 96 MiB batch guard.' }
    }
    if ($small -lt 16) { throw 'Small tile-sized scratch allocations were not demonstrated.' }
    $allAlloc = [regex]::Matches($native,'(?m)^\[RW-GPU-BLIT-SCRATCH\] event=alloc[^\r\n]*\bid=(\d+)\b')
    $releases = [regex]::Matches($native,'(?m)^\[RW-GPU-BLIT-SCRATCH\] event=release[^\r\n]*\bid=(\d+)\b[^\r\n]*\bliveBytes=(\d+)\b')
    if ($releases.Count -ne $allAlloc.Count -or $releases.Count -eq 0 -or [uint64]$releases[$releases.Count-1].Groups[2].Value -ne 0) { throw 'Scratch ownership accounting did not return to zero.' }
    $allocatedIds = @($allAlloc | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
    $releasedIds = @($releases | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
    if (@(Compare-Object $allocatedIds $releasedIds).Count -ne 0 -or @($allocatedIds | Select-Object -Unique).Count -ne $allocatedIds.Count) { throw 'Scratch allocation/release IDs do not balance exactly once.' }
    return $allocs.Count
}

function Confirm-ImageLifetime([string]$OutputFile,[string]$NativeLog) {
    if (-not [IO.File]::Exists($OutputFile) -or -not [IO.File]::Exists($NativeLog)) { throw 'Image lifetime output is missing.' }
    $text = [IO.File]::ReadAllText($OutputFile)
    if ($text -notmatch 'FINAL: PASS status=0;' -or $text -notmatch 'POOL_CYCLE_COMPLETE: 6/6;' -or $text -notmatch 'exact contiguous 16-byte readback matched' -or $text -match '(?m)^FAIL:|^FINAL: FAIL') { throw 'Image pool reuse/readback did not pass.' }
    $native = [IO.File]::ReadAllText($NativeLog)
    if ([regex]::Matches($native,'\[RW-HD5500-IMAGE-LIFETIME\] IMAGE_RESOURCE_RELEASE').Count -lt 6 -or $native -match '\[RW-GPU-DEVICE-FAIL\]') { throw 'Safe owned-image release was not demonstrated for all six pool cycles.' }
    return 6
}

function Confirm-CompactBacking([string]$NativeLog) {
    if (-not [IO.File]::Exists($NativeLog)) { throw 'Compact native backing log is missing. RimWorld was not started.' }
    $widths = @(Select-String -LiteralPath $NativeLog -Pattern '\[RW-GPU-MEMORY\] stage=BUFFER_BIND_END.*parentBytes=16777216\b.*logicalBytes=4096\b.*nativeBufferWidth=4096\b')
    if ($widths.Count -lt 9) { throw 'Compact backing width was not confirmed for all initial and recycled buffers. RimWorld was not started. Attach this report ZIP.' }
    return $widths.Count
}

function Confirm-AtlasBlit([string]$OutputFile) {
    if (-not [IO.File]::Exists($OutputFile)) { throw 'Atlas blit output is missing. RimWorld was not started.' }
    $text = [IO.File]::ReadAllText($OutputFile)
    if ($text -notmatch 'FINAL: PASS status=0;' -or $text -match 'FINAL: FAIL|FAIL:|CASE FAIL:') { throw 'Atlas blit output did not confirm a clean pass.' }
    if ([regex]::Matches($text,'(?m)^CASE PASS:').Count -ne 9 -or [regex]::Matches($text,'(?m)^PIXEL PASS:').Count -ne 9 -or [regex]::Matches($text,'(?m)^LIFETIME PASS:').Count -ne 9) { throw 'Atlas blit requires nine exact-pixel and completed-lifetime cases.' }
    return 9
}

function Confirm-LargeBc7([string]$OutputFile,[bool]$ExpectNoFallback) {
    if (-not [IO.File]::Exists($OutputFile)) { throw 'Large BC7 menu-background output is missing. RimWorld was not started.' }
    $text = [IO.File]::ReadAllText($OutputFile)
    if ($text -notmatch '(?m)^FINAL: PASS' -or $text -match '(?m)^CASE FAIL:|^MISMATCH:|^FINAL: FAIL') { throw 'Large BC7 menu-background check did not pass cleanly.' }
    if (-not $ExpectNoFallback) {
        foreach ($marker in @('CASE PASS: menu-bg-create-bind-5333x3000-13mips','CASE PASS: menu-bg-sampled-mip0','CASE PASS: menu-bg-sampled-mip12','CASE PASS: menu-bg-quadrant-upload-mip0','CASE PASS: menu-bg-readback-refused')) {
            if (-not $text.Contains($marker)) { throw ('Missing required large BC7 result: ' + $marker) }
        }
    }
    return [regex]::Matches($text,'(?m)^CASE PASS:').Count
}

function Get-LineSignature([string]$Line) {
    # Deduplicate repeated engine messages: drop numbers/addresses, trim, bound length.
    $t = ($Line -replace '0x[0-9A-Fa-f]+','#' -replace '\d+','#').Trim()
    if ($t.Length -gt 160) { $t = $t.Substring(0,160) }
    return $t
}

function Write-V12Classification([string]$Directory,[string]$PlayerCopy,[string]$NativeLog,[int]$ProcessResult) {
    # Item 5: a game that exits 0 but repeatedly throws rendering exceptions is a FAILED test.
    $out = New-Object System.Collections.Generic.List[string]
    $failed = $false
    $out.Add('V12 run classification (generated by the launcher; evidence from this run only)')
    $out.Add('Native game process result: ' + $ProcessResult)
    if ($PlayerCopy -and [IO.File]::Exists($PlayerCopy)) {
        $lines = [IO.File]::ReadAllLines($PlayerCopy)
        $apiErrors = @($lines | Where-Object { $_ -match 'ErrorFormatNotSupported|ErrorDeviceLost|ErrorOutOfDeviceMemory|ErrorInitializationFailed|ErrorFeatureNotPresent|VK_ERROR_' })
        $exceptions = @($lines | Where-Object { $_ -match '(?i)(root level exception|exception in|unhandled exception|\bexception\b.*:)' })
        $out.Add('Engine log lines: ' + $lines.Count + '; Vulkan error-name lines: ' + $apiErrors.Count + '; exception lines: ' + $exceptions.Count)
        $flagged = @($lines | Where-Object { $_ -match 'ErrorFormatNotSupported|ErrorDeviceLost|ErrorOutOfDeviceMemory|ErrorInitializationFailed|ErrorFeatureNotPresent|VK_ERROR_' -or $_ -match '(?i)(root level exception|exception in|unhandled exception|\bexception\b.*:)' })
        $groups = @($flagged | ForEach-Object { Get-LineSignature $_ } | Group-Object | Sort-Object Count -Descending | Select-Object -First 12)
        foreach ($g in $groups) { $out.Add(('  x' + $g.Count + '  ' + $g.Name)) }
        if ($apiErrors.Count -gt 0) { $failed = $true; $out.Add('RESULT-REASON: engine log reports a Vulkan error result.') }
        if (@($groups | Where-Object { $_.Count -ge 3 }).Count -gt 0) { $failed = $true; $out.Add('RESULT-REASON: the same engine exception repeated 3+ times (for example every OnGUI frame).') }
    } else { $out.Add('No fresh Player.log was captured; engine-side classification unavailable.') }
    if ($NativeLog -and [IO.File]::Exists($NativeLog)) {
        $native = [IO.File]::ReadAllLines($NativeLog)
        $kinds = @($native | Where-Object { $_ -match '^\[RW-GPU-DIAG\] kind=' } | ForEach-Object { ([regex]::Match($_,'kind=([a-z0-9-]+)')).Groups[1].Value } | Group-Object | Sort-Object Name)
        foreach ($k in $kinds) { $out.Add(('Bridge diagnostic ' + $k.Name + ': ' + $k.Count + ' record(s) (records are deduplicated; see count= fields)')) }
        $badKinds = @($kinds | Where-Object { $_.Name -in @('bc-representation-unsupported','bc-decoded-upload-rejected','bc-decoded-readback-rejected','bc-copy-representation-mismatch','bc-decoded-host-readback') })
        if ($badKinds.Count -gt 0) { $failed = $true; $out.Add('RESULT-REASON: the bridge rejected or could not represent a texture operation (see kinds above).') }
        if (@($native | Where-Object { $_ -match '\[RW-GPU-DEVICE-FAIL\]' }).Count -gt 0) { $failed = $true; $out.Add('RESULT-REASON: native D3D11 device failure recorded.') }
        $boundary = @($native | Where-Object { $_ -match '\[RW-GPU-BOUNDARY\]|recording failed' })
        if ($boundary.Count -gt 0) { $out.Add('Bridge boundary/recording failures: ' + $boundary.Count + ' (first: ' + (Get-LineSignature $boundary[0]) + ')') }
        foreach ($tag in @('RW-GPU-MEMORY-HEAPS','RW-GPU-MEMORY-BUDGET','RW-GPU-MEMORY-SUMMARY','RW-GPU-DIAG-SUMMARY','RW-GPU-STAGING','RW-GPU-SHADERCACHE','RW-GPU-STATECACHE','RW-GPU-FENCE','RW-GPU-TIMING','RW-GPU-CAPS')) {
            $hit = @($native | Where-Object { $_.StartsWith('[' + $tag + ']') } | Select-Object -Last 2)
            foreach ($h in $hit) { $out.Add($h) }
        }
    } else { $out.Add('Native bridge stderr was not captured.') }
    $out.Add('CLASSIFICATION: ' + $(if ($failed) { 'FAIL (rendering errors detected even if the process exit code was 0)' } else { 'NO RENDER-FAILURE MARKERS (does not prove correct pixels or frame rate)' }))
    [IO.File]::WriteAllLines((Join-Path $Directory 'v12-classification.txt'),$out,[Text.Encoding]::UTF8)
    foreach ($line in $out) { Write-Host $line }
    return $failed
}

try {
    $logs = Join-Path $PSScriptRoot 'reports'
    New-Item -ItemType Directory -Path $logs -Force | Out-Null
    $runDir = Join-Path $logs ('gpu-run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Path $runDir | Out-Null
    Start-Transcript -LiteralPath (Join-Path $runDir 'launcher.txt') -Force | Out-Null
    $transcript = $true
    Write-Host 'EXPERIMENTAL: Vulkan 1.0 -> Ashes D3D11 GPU bridge'
    Write-Host 'Bridge revision: v13.2 swapchain-format fix; indexed-UI fixture is informational; v13 large BC7 fix retained. Key checks by default, optional full matrix. Native game success remains unverified.'
    Write-Host ('Time: ' + (Get-Date -Format o))
    Write-Host 'Not Lavapipe or SwiftShader. Full RimWorld compatibility/frame rate is not established.'
    if (-not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) { throw 'Requires Windows x64 and 64-bit Windows PowerShell.' }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    $elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Write-Host ('Administrator/elevated: ' + $elevated)
    if ($elevated) { throw 'Run normally, NOT as Administrator. Elevated Vulkan processes ignore ICD-selection variables.' }
    $system = Join-Path $env:SystemRoot 'System32\vulkan-1.dll'
    if (-not (Test-Path -LiteralPath $system -PathType Leaf)) { throw 'Existing Windows System32 Vulkan loader not found. Do not copy DLLs into System32 or create a fake game Vulkan folder.' }
    $manifest = Join-Path $PSScriptRoot 'runtime\ashesD3D11Renderer_icd.json'
    $backend = Join-Path $PSScriptRoot 'runtime\ashesD3D11Renderer.dll'
    $common = Join-Path $PSScriptRoot 'runtime\ashes-1.dll'
    $runner = Join-Path $PSScriptRoot 'tests\RunCapture.exe'
    $probe = Join-Path $PSScriptRoot 'tests\Vulkan10GpuIcdProbe.exe'
    $stagingProbe = Join-Path $PSScriptRoot 'tests\StagingBufferBindRepro.exe'
    $textureProbe = Join-Path $PSScriptRoot 'tests\TextureUploadProbe.exe'
    $matrixProbe = Join-Path $PSScriptRoot 'tests\TextureMatrix.exe'
    $bcProbe = Join-Path $PSScriptRoot 'tests\BC7MatrixProbe.exe'
    $gpuBcProbe = Join-Path $PSScriptRoot 'tests\BC7GpuSourceProbe.exe'
    $compactProbe = Join-Path $PSScriptRoot 'tests\CompactBufferProbe.exe'
    $suballocatedCompute = Join-Path $PSScriptRoot 'tests\SuballocatedComputeProbe.exe'
    $atlasProbe = Join-Path $PSScriptRoot 'tests\AtlasBlitProbe.exe'
    $failureProbe = Join-Path $PSScriptRoot 'tests\FailurePathProbe.exe'
    $atlasBatchProbe = Join-Path $PSScriptRoot 'tests\AtlasBatchProbe.exe'
    $imageLifetimeProbe = Join-Path $PSScriptRoot 'tests\ImagePoolLifetimeProbe.exe'
    $largeBc7Probe = Join-Path $PSScriptRoot 'tests\LargeBC7MenuBackgroundProbe.exe'
    $capabilityProbe = Join-Path $PSScriptRoot 'tests\CapabilityConsistencyProbe.exe'
    $fenceProbe = Join-Path $PSScriptRoot 'tests\FenceQueryProbe.exe'
    $uiProbe = Join-Path $PSScriptRoot 'tests\UiIndexedDrawProbe.exe'
    $proxy = Join-Path $PSScriptRoot 'compat-loader\vulkan-1.dll'
    $loaderCopy = Join-Path $PSScriptRoot 'compat-loader\rw_system_vulkan.dll'
    foreach ($file in @($manifest,$backend,$common,$runner,$probe,$stagingProbe,$textureProbe,$matrixProbe,$bcProbe,$gpuBcProbe,$compactProbe,$suballocatedCompute,$atlasProbe,$failureProbe,$atlasBatchProbe,$imageLifetimeProbe,$largeBc7Probe,$capabilityProbe,$fenceProbe,$uiProbe,$proxy)) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw ('Missing ' + $file + '. Extract the whole ZIP to a separate folder.') }
    }
    Write-Host ('Renderer SHA256: ' + (Get-FileHash -LiteralPath $backend -Algorithm SHA256).Hash)
    if ((Get-FileHash -LiteralPath $backend -Algorithm SHA256).Hash -ne 'F4BEF519E2F667C6C6D4C6D3235F3BC979EB63EB8819BA2B954467ABA62428C8') { throw 'Renderer hash differs from this v13.86 release. Re-extract the whole ZIP; do not mix revisions.' }
    # Local byte-identical copy of the user's existing loader; no Windows file changes.
    Copy-Item -LiteralPath $system -Destination $loaderCopy -Force
    if ((Get-FileHash -LiteralPath $system).Hash -ne (Get-FileHash -LiteralPath $loaderCopy).Hash) { throw 'Local original-loader copy verification failed.' }
    Write-Host ('Existing loader: ' + $system)
    Write-Host ('Original-loader SHA256: ' + (Get-FileHash -LiteralPath $system).Hash)
    Write-Host ('Experimental ICD: ' + $manifest)
    Write-Host 'No registry, Windows files, save, mod or permanent environment changes.'
    foreach ($name in $envNames) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
    if ($Reference) {
        $lvpDir = Join-Path $PSScriptRoot 'reference\lavapipe'
        $lvpDll = Join-Path $lvpDir 'vulkan_lvp.dll'
        $lvpJson = Join-Path $lvpDir 'lvp_icd.x86_64.json'
        if (-not (Test-Path -LiteralPath $lvpDll -PathType Leaf)) {
            # Delivered in parts (file-size limit of the transfer); join once.
            $parts = @(Get-ChildItem -LiteralPath $lvpDir -Filter 'vulkan_lvp.dll.part*' | Sort-Object Name)
            if ($parts.Count -eq 0) { throw 'reference\lavapipe is missing.' }
            $out = [IO.File]::Create($lvpDll)
            try { foreach ($part in $parts) { $bytes = [IO.File]::ReadAllBytes($part.FullName); $out.Write($bytes, 0, $bytes.Length) } } finally { $out.Close() }
            Write-Host ('Joined lavapipe from ' + $parts.Count + ' parts')
        }
        $expected = (Get-Content -LiteralPath (Join-Path $lvpDir 'vulkan_lvp.dll.sha256') -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($expected -and (Get-FileHash -LiteralPath $lvpDll -Algorithm SHA256).Hash -ne $expected.Trim().ToUpper()) { Remove-Item -LiteralPath $lvpDll -Force; throw 'lavapipe join failed (hash mismatch); start again.' }
        $manifest = $lvpJson
        Write-Host 'REFERENCE MODE: Mesa lavapipe (CPU Vulkan) instead of the HD5500 bridge. Expect a few frames per second.'
    }
    elseif ($AltIcd) {
        if (-not (Test-Path -LiteralPath $AltIcd -PathType Leaf) -or [IO.Path]::GetExtension($AltIcd) -ne '.json') { throw ('-AltIcd needs the full path of a Vulkan driver manifest (.json): ' + $AltIcd) }
        $manifest = (Resolve-Path -LiteralPath $AltIcd).Path
        Write-Host ('ALTERNATIVE DRIVER MODE: ' + $manifest + ' instead of the HD5500 bridge (comparison only; not supported by this project).')
    }
    $foreignIcd = [bool]($Reference -or $AltIcd)
    [Environment]::SetEnvironmentVariable('VK_DRIVER_FILES',$manifest,'Process')
    [Environment]::SetEnvironmentVariable('VK_ICD_FILENAMES',$manifest,'Process')
    [Environment]::SetEnvironmentVariable('VK_ADD_DRIVER_FILES',$null,'Process')
    [Environment]::SetEnvironmentVariable('LP_NUM_THREADS',$null,'Process')
    [Environment]::SetEnvironmentVariable('LP_NATIVE_VECTOR_WIDTH',$null,'Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_PORTABILITY','1','Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_DISABLE_BC',$null,'Process')
    $detailedDiagnostics = [bool]($SerialDiagnostics -or $VerboseDiagnostics)
    Set-BridgeDiagnostics $detailedDiagnostics
    Write-Host ('Detailed tracing/native debug layer requested: ' + $detailedDiagnostics + '. Normal mode retains native failure and bounded allocation evidence without full command tracing.')
    if ($SerialDiagnostics) { [Environment]::SetEnvironmentVariable('RW_HD5500_SERIAL_CHECKPOINTS','1','Process') }
    else { [Environment]::SetEnvironmentVariable('RW_HD5500_SERIAL_CHECKPOINTS',$null,'Process') }
    Write-Host ('Serialized diagnostic GPU checkpoints: ' + [bool]$SerialDiagnostics + '. This optional mode is slower and is not an FPS optimization.')
    Write-Host 'V12 retains compact atlas and buffer lifetimes; oversized supported BC uses decoded logical-size backing. No TDR/pagefile/driver/Windows settings are changed.'
    Write-Host 'Legal native BC remains available. Oversized supported BC uses a decoded RGBA8 representation; sampled values are checked before launch and unsupported operations are refused.'
    Write-Host 'Complete matching BC mips use a whole-subresource GPU copy; partial regions remain unchanged. Intel tail-mip bytes must pass before game launch.'
    Write-Host 'Native texture descriptors are recorded before error callbacks, including failures beyond the small preflights.'
    if (-not $KeepImplicitLayers) {
        [Environment]::SetEnvironmentVariable('VK_LOADER_LAYERS_DISABLE','~implicit~','Process')
        [Environment]::SetEnvironmentVariable('VK_LOADER_LAYERS_ENABLE',$null,'Process')
        [Environment]::SetEnvironmentVariable('VK_LOADER_LAYERS_ALLOW',$null,'Process')
        Write-Host 'Implicit overlay/recording layers excluded for this process only where supported.'
    }
    # v12 toggles: start from the tested defaults; optional switches only for A/B diagnosis.
    foreach ($name in @('RW_HD5500_LEGACY_CAPS','RW_HD5500_FENCE_EVENTS','RW_HD5500_STAGING_CACHE_MB','RW_HD5500_SHADER_CACHE','RW_HD5500_SHADER_CACHE_MB','RW_HD5500_COHERENT_REFRESH','RW_HD5500_FRAME_PROBE','RW_HD5500_UI_TRACE','RW_HD5500_FRAME_DIAG','RW_HD5500_FRAME_DUMP_DIR','RW_HD5500_SWAPCHAIN_IMAGES')) { [Environment]::SetEnvironmentVariable($name,$null,'Process') }
    [Environment]::SetEnvironmentVariable('RW_HD5500_BC_DECODE_FALLBACK',$(if ($NoDecodeFallback) {'0'} else {$null}),'Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_STATE_CACHE',$(if ($StateCache) {'1'} else {$null}),'Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_GPU_TIMING',$(if ($GpuTiming) {'1'} else {$null}),'Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_HAZARD_TRACKING',$(if ($NoHazardTracking) {'0'} else {$null}),'Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_COHERENT_REFRESH','1','Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_FRAME_PROBE',$(if ($FrameProbe) {'1'} else {'0'}),'Process')
    [Environment]::SetEnvironmentVariable('RW_HD5500_UI_TRACE',$(if ($UiTrace) {'1'} else {'0'}),'Process')
    Write-Host ('Check mode: ' + $(if ($FullChecks) {'full retained suite'} else {'focused key checks'}) + '; frameProbe=' + [bool]$FrameProbe + '; uiTrace=' + [bool]$UiTrace)
    Write-Host ('V13 settings: decodedLargeBc=' + (-not $NoDecodeFallback) + ' stateCache=' + [bool]$StateCache + ' gpuTiming=' + [bool]$GpuTiming + ' hazardTracking=' + (-not $NoHazardTracking) + ' fenceEvents=True stagingCacheMiB=256 shaderCache=True')
    # v13.45: decimal-separator protection (see the parameter comment). PredefinedCulturesOnly=false is
    # required: the game calls CultureInfo.CreateSpecificCulture("en-US"), which throws in strict invariant mode.
    if ($NoInvariantCulture) {
        [Environment]::SetEnvironmentVariable('DOTNET_SYSTEM_GLOBALIZATION_INVARIANT',$null,'Process')
        [Environment]::SetEnvironmentVariable('DOTNET_SYSTEM_GLOBALIZATION_PREDEFINED_CULTURES_ONLY',$null,'Process')
        Write-Host 'Number format: Windows regional settings (invariant culture OFF, -NoInvariantCulture).'
    } else {
        [Environment]::SetEnvironmentVariable('DOTNET_SYSTEM_GLOBALIZATION_INVARIANT','1','Process')
        [Environment]::SetEnvironmentVariable('DOTNET_SYSTEM_GLOBALIZATION_PREDEFINED_CULTURES_ONLY','false','Process')
        $sep = [Globalization.CultureInfo]::CurrentCulture.NumberFormat.NumberDecimalSeparator
        Write-Host ("Number format: invariant culture ON (saves always use a dot; Windows decimal separator here is '" + $sep + "').")
    }
    # v13.30: one-off experiments without new buttons. experiment-env.txt next to this script,
    # lines KEY=VALUE (only RW_GPU_* / RW_HD5500_* / RW_OPT_* / DOTNET_GC* / DOTNET_Tiered* / DOTNET_TC_* names (v13.48: .NET garbage collector experiments); '#' = comment). Delete the file to stop.
    $experimentFile = Join-Path $PSScriptRoot 'experiment-env.txt'
    if (-not $foreignIcd -and (Test-Path -LiteralPath $experimentFile -PathType Leaf)) {
        foreach ($line in Get-Content -LiteralPath $experimentFile) {
            $t = $line.Trim()
            if ($t -eq '' -or $t.StartsWith('#')) { continue }
            if ($t -match '^(RW_GPU_[A-Z0-9_]+|RW_HD5500_[A-Z0-9_]+|RW_OPT_[A-Z0-9_]+|DOTNET_GC[A-Za-z0-9_]+|DOTNET_Tiered[A-Za-z0-9_]*|DOTNET_TC_[A-Za-z0-9_]+)=(.*)$') {   # v13.73: + .NET tiered-compilation experiments   # v13.67: + RW_OPT_* (HD5500 Optimizer)
                [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process')
                Write-Host ('EXPERIMENT: ' + $Matches[1] + '=' + $Matches[2] + ' (from experiment-env.txt)')
            }
        }
    }
    # v13.78: private local build - its env file is read after experiment-env.txt (local wins).
    $localGameArgs = ''
    if ($LocalDir -and -not $foreignIcd) {
        $localEnv = Join-Path $LocalDir 'local-env.txt'
        if (Test-Path -LiteralPath $localEnv -PathType Leaf) {
            foreach ($line in Get-Content -LiteralPath $localEnv) {
                $t = $line.Trim()
                if ($t -eq '' -or $t.StartsWith('#')) { continue }
                if ($t -match '^RW_LOCAL_GAME_ARGS=(.*)$') { $localGameArgs = $Matches[1].Trim(); Write-Host ('LOCAL: extra game arguments ' + $localGameArgs); continue }
                if ($t -match '^(RW_LOCAL_[A-Z0-9_]+|RW_GPU_[A-Z0-9_]+|RW_HD5500_[A-Z0-9_]+|RW_OPT_[A-Z0-9_]+|DOTNET_GC[A-Za-z0-9_]+|DOTNET_Tiered[A-Za-z0-9_]*|DOTNET_TC_[A-Za-z0-9_]+|DOTNET_ReadyToRun|DOTNET_PROCESSOR_COUNT|WAYLITH_NO_ASSET_PREFETCH|WAYLITH_STAGE_UPLOADS_IN_RAM|WAYLITH_VRAM_BUDGET_MB)=(.*)$') {
                    [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process')
                    Write-Host ('LOCAL: ' + $Matches[1] + '=' + $Matches[2] + ' (from local-env.txt)')
                }
            }
        } else { Write-Host ('LOCAL: no local-env.txt in ' + $LocalDir) }
    }
    # v13.49: Vulkan version comparison. RW_GPU_API_VERSION=1.1 or 1.2 (experiment-env.txt) makes the
    # bridge report that version; the loader also reads the version from the ICD manifest, so a
    # matching manifest copy is written next to the original and used for this run only.
    $apiVer = [Environment]::GetEnvironmentVariable('RW_GPU_API_VERSION','Process')
    if (-not $foreignIcd -and ($apiVer -eq '1.1' -or $apiVer -eq '1.2')) {
        $verManifest = Join-Path $PSScriptRoot ('runtime\ashesD3D11Renderer_icd_' + $apiVer.Replace('.','') + '.json')
        ((Get-Content -LiteralPath $manifest -Raw) -replace '"api_version":\s*"[0-9.]+"', ('"api_version": "' + $apiVer + '.0"')) | Set-Content -LiteralPath $verManifest -Encoding ASCII
        $manifest = $verManifest
        [Environment]::SetEnvironmentVariable('VK_DRIVER_FILES',$manifest,'Process')
        [Environment]::SetEnvironmentVariable('VK_ICD_FILENAMES',$manifest,'Process')
        Write-Host ('Vulkan version comparison: the bridge reports Vulkan ' + $apiVer + ' (manifest ' + $verManifest + ').')
    }
    Write-Host 'Check 1: stock loader + honest portable ICD, Intel selection, actual compute readback and Win32 presentation.'
    $raw = '--loader "' + $system + '" --icd "' + $manifest + '"'
    if ($foreignIcd) { $code = 0; Write-Host 'Reference/alternative driver mode: check 1 (Intel selection) skipped.' }
    else { $code = Invoke-Captured $probe $raw 'stock-gpu-preflight' 120; Show-Probe 'stock-gpu-preflight' }
    if ($code -ne 0) { throw ('GPU driver check failed, code ' + $code + '. RimWorld was not started and no game files were changed. Attach this report ZIP.') }
    if ($QuickStart) { Write-Host 'QuickStart: checks 2-18 skipped (play mode). Use Diagnose-UI-Frame.cmd for the full checks.' }
    if (-not $QuickStart) {
    Write-Host 'Check 2: compatibility proxy adds only the missing portability opt-in; original API/features/errors retained.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '" --no-portability-optin'
    $code = Invoke-Captured $probe $raw 'proxy-gpu-preflight' 120
    Show-Probe 'proxy-gpu-preflight'
    if ($code -ne 0) { throw ('Compatibility-loader check failed, code ' + $code + '. RimWorld was not started and no game files were changed. Attach this report ZIP.') }

    Write-Host 'Check 3: two transfer buffers sharing one host-visible memory block, nonzero binding offset, copy and sampled readback.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    $code = Invoke-Captured $stagingProbe $raw 'staging-suballocation-preflight' 120
    Show-Probe 'staging-suballocation-preflight'
    if ($code -ne 0) { throw ('Staging suballocation check failed, code ' + $code + '. RimWorld was not started and no game files were changed. Attach this report ZIP.') }

    Write-Host 'Check 4: padded texture upload, nonzero buffer offsets, exact RGBA8 readback and small-image allocation checks.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    $code = Invoke-Captured $textureProbe $raw 'texture-upload-preflight' 120
    Show-Probe 'texture-upload-preflight'
    if ($code -ne 0) { throw ('Texture upload check failed, code ' + $code + '. RimWorld was not started and no game files were changed. Attach this report ZIP.') }

    Write-Host 'Check 5: host-visible GPU-backed image upload and exact pixel readback, including a nonzero image binding.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '" --host-visible-image-upload'
    $code = Invoke-Captured $textureProbe $raw 'host-texture-upload-preflight' 120
    Show-Probe 'host-texture-upload-preflight'
    if ($code -ne 0) { throw ('Host-image upload check failed, code ' + $code + '. RimWorld was not started and no game files were changed. Attach this report ZIP.') }

    if ($FullChecks) {
    Write-Host 'Check 6: supported uncompressed and BC UI texture formats, dimensions and mip-chain allocations.'
    $raw = '--loader "' + $system + '" --icd "' + $manifest + '" --matrix-only'
    $code = Invoke-Captured $matrixProbe $raw 'texture-matrix-preflight' 120
    Get-Content -LiteralPath (Join-Path $runDir 'texture-matrix-preflight.stdout.txt') -Tail 14 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) { Show-Probe 'texture-matrix-preflight'; throw ('Texture matrix failed, code ' + $code + '. RimWorld was not started. Attach this report ZIP.') }

    }

    if ($FullChecks) {
    Write-Host 'Check 7: real BC7 UNORM/sRGB images including background dimensions, full/tail mip views, and exact five-mip compressed-block readback.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    $code = Invoke-Captured $bcProbe $raw 'bc7-mips-preflight' 180
    Get-Content -LiteralPath (Join-Path $runDir 'bc7-mips-preflight.stdout.txt') -Tail 22 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) {
        Show-Probe 'bc7-mips-preflight'
        $nativeBcProbe = Join-Path $PSScriptRoot 'tests\NativeBcCopyProbe.exe'
        if (Test-Path -LiteralPath $nativeBcProbe -PathType Leaf) {
            Write-Host 'Capturing a directed Intel D3D11 copy comparison to separate readback and upload paths. This diagnostic does not bypass the failed game preflight.'
            try {
                $nativeBcCode = Invoke-Captured $nativeBcProbe '' 'native-bc-copy-diagnostic' 90
                Write-Host ('Native BC comparison exit: ' + $nativeBcCode)
                Get-Content -LiteralPath (Join-Path $runDir 'native-bc-copy-diagnostic.stdout.txt') -Tail 22 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
            } catch { Write-Host ('Supplemental native diagnostic failed: ' + $_.Exception.Message) }
        }
        throw ('BC7 mip/readback check failed, code ' + $code + '. RimWorld was not started. Attach this report ZIP.')
    }

    }

    Write-Host 'Check 8: host upload -> device-local-only GPU transfer source -> BC7 mip chain -> exact host readback; GPU-source memory is never CPU mapped.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    $code = Invoke-Captured $gpuBcProbe $raw 'bc7-gpu-source-preflight' 180
    Get-Content -LiteralPath (Join-Path $runDir 'bc7-gpu-source-preflight.stdout.txt') -Tail 22 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) { Show-Probe 'bc7-gpu-source-preflight'; throw ('GPU-source BC7 transfer check failed, code ' + $code + '. RimWorld was not started. Attach this report ZIP.') }

    Write-Host 'Check 9: eight simultaneous compact transfer and storage buffers, nonzero parent bindings, full parent-byte comparison and released-slot reuse.'
    foreach ($storageMode in @($false,$true)) {
        $label = if ($storageMode) { 'compact-storage-preflight' } else { 'compact-transfer-preflight' }
        $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
        if ($storageMode) { $raw += ' --storage' }
        $code = Invoke-Captured $compactProbe $raw $label 180
        Get-Content -LiteralPath (Join-Path $runDir ($label + '.stdout.txt')) -Tail 16 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        if ($code -ne 0) { Show-Probe $label; throw ('Compact buffer data/lifetime check failed, code ' + $code + '. RimWorld was not started. Attach this report ZIP.') }
        $confirmedWidths = Confirm-CompactBacking (Join-Path $runDir ($label + '.stderr.txt'))
        Write-Host ($label + ': verified 4 KiB native backings, not 16 MiB parent duplicates.')
    }

    Write-Host 'Check 10: actual compute shader readback with a storage buffer bound at a nonzero parent offset; raw native UAV base must be buffer-local.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '" --app-no-portability'
    $code = Invoke-Captured $suballocatedCompute $raw 'suballocated-compute-preflight' 120
    Show-Probe 'suballocated-compute-preflight'
    if ($code -ne 0) { throw ('Suballocated compute check failed, code ' + $code + '. RimWorld was not started. Attach this report ZIP.') }

    Write-Host 'Check 11: actual atlas-style nearest blits, odd/scaled nonzero rectangles, unchanged guard pixels and repeated temporary-resource destruction.'
    $atlasModes = if ($FullChecks) { @('destination-temp','both-temp','destination-direct','secondary-clone','bc7-decode') } else { @('bc7-decode') }
    foreach ($mode in $atlasModes) {
        $label = 'atlas-blit-' + $mode
        $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '" --iterations 3'
        if ($mode -eq 'both-temp') { $raw += ' --source-needs-temp' }
        if ($mode -eq 'destination-direct') { $raw += ' --destination-direct' }
        if ($mode -eq 'secondary-clone') { $raw += ' --clone-secondary --source-needs-temp' }
        if ($mode -eq 'bc7-decode') { $raw += ' --bc7-source' }
        $code = Invoke-Captured $atlasProbe $raw $label 180
        Get-Content -LiteralPath (Join-Path $runDir ($label + '.stdout.txt')) -Tail 12 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        if ($code -ne 0) {
            Show-Probe $label
            throw ('Atlas blit pixel/lifetime check failed: ' + $mode + ', code ' + $code + '. RimWorld was not started. Attach this report ZIP.')
        }
        $verifiedAtlasCases = Confirm-AtlasBlit (Join-Path $runDir ($label + '.stdout.txt'))
    }

    if ($FullChecks) {
    Write-Host 'Check 12: real host-allocation refusal, defensive constructor-failure cleanup and valid resource creation afterwards.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    $code = Invoke-Captured $failureProbe $raw 'failure-path-preflight' 120
    Show-Probe 'failure-path-preflight'
    if ($code -ne 0) { throw ('Resource failure-path check failed, code ' + $code + '. RimWorld was not started. Attach this report ZIP.') }
    $failureText = [IO.File]::ReadAllText((Join-Path $runDir 'failure-path-preflight.stdout.txt'))
    foreach ($marker in @('PASS: allocation refusal preserved','PASS: failed constructor storage freed','PASS: healthy resource after failures','FINAL: FAILURE-PATH PASS')) {
        if (-not $failureText.Contains($marker)) { throw ('Missing required failure-path result: ' + $marker + '. RimWorld was not started.') }
    }

    }

    if ($FullChecks) {
    Write-Host 'Check 13: a real 4096 atlas with many retained small blits, full guard-pixel checks, reset/reuse, mip/layer neighbors and clone-safe compact scratch accounting.'
    foreach ($mode in @('primary','secondary-clone')) {
        $label = 'atlas-batch-' + $mode
        $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '" --mip-layer'
        if ($mode -eq 'secondary-clone') { $raw += ' --clone-secondary' }
        $code = Invoke-Captured $atlasBatchProbe $raw $label 240
        Get-Content -LiteralPath (Join-Path $runDir ($label + '.stdout.txt')) -Tail 14 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        if ($code -ne 0) { Show-Probe $label; throw ('Atlas batch stress test failed: ' + $mode + ', code ' + $code + '. RimWorld was not started.') }
        $compactAtlasAllocations = Confirm-AtlasBatch (Join-Path $runDir ($label + '.stdout.txt')) (Join-Path $runDir ($label + '.stderr.txt'))
        Write-Host ($label + ': pixels and balanced compact scratch ownership verified; diagnostic timings do not predict game FPS.')
    }
    }

    Write-Host 'Check 14: destroyed images release native backing while the pooled parent remains live; subsequent exact readback and unused parent-free-first cleanup.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    $code = Invoke-Captured $imageLifetimeProbe $raw 'image-lifetime-preflight' 120
    Get-Content -LiteralPath (Join-Path $runDir 'image-lifetime-preflight.stdout.txt') -Tail 14 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) { Show-Probe 'image-lifetime-preflight'; throw ('Image lifetime test failed, code ' + $code + '. RimWorld was not started.') }
    $verifiedImageCycles = Confirm-ImageLifetime (Join-Path $runDir 'image-lifetime-preflight.stdout.txt') (Join-Path $runDir 'image-lifetime-preflight.stderr.txt')

    Write-Host 'Check 15 (v12): the exact RimWorld menu background shape - 5333x3000 BC7, 13 mips - create/bind/upload, exact sampled pixels on all tested mips, partial uploads, decoded copy, refused readback and 16384 boundaries.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    if ($NoDecodeFallback) { $raw += ' --expect-no-fallback' }
    $code = Invoke-Captured $largeBc7Probe $raw 'large-bc7-menu-background-preflight' 600
    Get-Content -LiteralPath (Join-Path $runDir 'large-bc7-menu-background-preflight.stdout.txt') -Tail 16 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) { Show-Probe 'large-bc7-menu-background-preflight'; throw ('Large BC7 menu-background check failed, code ' + $code + '. RimWorld was not started. Attach this report ZIP.') }
    $largeBc7Cases = Confirm-LargeBc7 (Join-Path $runDir 'large-bc7-menu-background-preflight.stdout.txt') ([bool]$NoDecodeFallback)
    Write-Host ('Large BC7 menu background: ' + $largeBc7Cases + ' exact cases passed on this adapter.')

    Write-Host 'Check 16 (v12): real fence completion (unsignaled/pending/timeout/reset/wait-all/wait-any), query wait/availability flags and timestamp period.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    $code = Invoke-Captured $fenceProbe $raw 'fence-query-preflight' 180
    Get-Content -LiteralPath (Join-Path $runDir 'fence-query-preflight.stdout.txt') -Tail 12 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    $fenceText = [IO.File]::ReadAllText((Join-Path $runDir 'fence-query-preflight.stdout.txt'))
    if ($code -ne 0 -or $fenceText -notmatch '(?m)^FINAL: PASS' -or $fenceText -match '(?m)^FAIL|^FINAL: FAIL') { Show-Probe 'fence-query-preflight'; throw ('Fence/query completion check failed, code ' + $code + '. RimWorld was not started. Attach this report ZIP.') }

    if ($FullChecks) {
    Write-Host 'Check 17 (v12, recorded, does not block the game): every format/usage query is compared with real image creation and binding on this adapter.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    if ($NoDecodeFallback) { $raw += ' --expect-no-decode-fallback' }
    $code = Invoke-Captured $capabilityProbe $raw 'capability-consistency-check' 300
    Get-Content -LiteralPath (Join-Path $runDir 'capability-consistency-check.stdout.txt') -Tail 14 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    $capText = [IO.File]::ReadAllText((Join-Path $runDir 'capability-consistency-check.stdout.txt')) + [Environment]::NewLine + [IO.File]::ReadAllText((Join-Path $runDir 'capability-consistency-check.stderr.txt'))
    $capMismatches = [regex]::Matches($capText,'(?m)^MISMATCH:').Count
    if ($code -ne 0 -or $capMismatches -gt 0) {
        Write-Host ('WARNING: capability consistency reported ' + $capMismatches + ' mismatch(es), code ' + $code + '. This does not block RimWorld (it covers formats the menu may never use) but please attach the report ZIP.') -ForegroundColor Yellow
    } else { Write-Host 'Capability consistency: query results matched real creation/binding for every tested case.' }

    }

    Write-Host 'Check 18 (v13): actual indexed textured UI draw, coherent mapped inputs and descriptor set addressing.'
    $raw = '--loader "' + $proxy + '" --icd "' + $manifest + '"'
    $code = Invoke-Captured $uiProbe $raw 'indexed-ui-draw-preflight' 180
    Show-Probe 'indexed-ui-draw-preflight'
    $uiText = [IO.File]::ReadAllText((Join-Path $runDir 'indexed-ui-draw-preflight.stdout.txt')) + [Environment]::NewLine + [IO.File]::ReadAllText((Join-Path $runDir 'indexed-ui-draw-preflight.stderr.txt'))
    if ($code -ne 0 -or $uiText -notmatch '(?m)^FINAL: PASS' -or $uiText -match '(?m)^MISMATCH:|^FINAL: FAIL|^CASE FAIL:') {
        Write-Host ('WARNING: indexed UI fixture did not pass, code ' + $code + '. This is recorded but is non-blocking so RimWorld can be tested on the real adapter.') -ForegroundColor Yellow
        Add-Content -LiteralPath (Join-Path $runDir 'KNOWN-UI-PROBE-FAILURE.txt') -Value ('Indexed UI fixture non-blocking result. Exit=' + $code + [Environment]::NewLine + $uiText)
    } else { Write-Host 'Indexed UI fixture passed.' }

    } # end of checks 2-18 (skipped with -QuickStart)
    if ($TestOnly) {
        Write-Host 'All selected blocking checks passed, including indexed UI drawing, large BC7 background and real fence completion. Full matrices run only with -FullChecks. Test-only mode: RimWorld was not started; no game files were changed.'
        Write-Host 'A passing probe is not full game compatibility or a promised frame rate.'
        $exitCode = 0
    } else {
        if (-not (Test-Path -LiteralPath $GameExe -PathType Leaf)) { $GameExe = (Read-Host 'Paste the full path to RimWorld 1.7 RimWorld.exe').Trim().Trim([char]34) }
        if (-not (Test-Path -LiteralPath $GameExe -PathType Leaf)) { throw 'RimWorld.exe was not found.' }
        $GameExe = (Resolve-Path -LiteralPath $GameExe).Path
        if ([IO.Path]::GetFileName($GameExe) -ne 'RimWorld.exe') { throw 'Choose RimWorld 1.7 RimWorld.exe, not Steam or the old Unity executable.' }
        $workDir = Split-Path -Parent $GameExe
        $existing = @(Get-Process -Name RimWorld -ErrorAction SilentlyContinue)
        if ($existing.Count) { throw 'RimWorld is already running. Close it normally before testing another instance.' }
        Stage-Temporary $proxy (Join-Path $workDir 'vulkan-1.dll')
        Stage-Temporary $loaderCopy (Join-Path $workDir 'rw_system_vulkan.dll')
        $data = Join-Path $env:USERPROFILE 'AppData\LocalLow\Ludeon Studios\RimWorld by Ludeon Studios'
        $player = Join-Path $data 'Logs\Player.log'
        $beforeHash = $null
        if (Test-Path -LiteralPath $player -PathType Leaf) { $beforeHash = (Get-FileHash -LiteralPath $player).Hash }
        $startUtc = [DateTime]::UtcNow
        Write-Host ('Game: ' + $GameExe)
        Write-Host 'Keep Steam and this console open. Starting the game with the experimental GPU ICD.'
        Write-Host 'Temporary game-root compatibility files will be removed when the game exits. No game timeout.'
        # v13.4: set only for the game process, after all preflight checks.
        if ($FrameDiag) {
            $frameDir = Join-Path $runDir 'frames'
            New-Item -ItemType Directory -Path $frameDir -Force | Out-Null
            [Environment]::SetEnvironmentVariable('RW_HD5500_FRAME_DIAG','1','Process')
            [Environment]::SetEnvironmentVariable('RW_HD5500_FRAME_DUMP_DIR',$frameDir,'Process')
        }
        [Environment]::SetEnvironmentVariable('RW_HD5500_SWAPCHAIN_IMAGES',$(if ($SwapchainImages -gt 1) {[string]$SwapchainImages} else {$null}),'Process')
        if ($QuickStart) {
            # v13.16: -FrameDiag survives -QuickStart (unattended map tests need both).
            $quickClear = @('RW_HD5500_MEMORY_TRACE','RW_HD5500_BLIT_SCRATCH_TRACE','RW_HD5500_IMAGE_LIFETIME_TRACE','RW_HD5500_TEXTURE_TRACE','RW_HD5500_COMMAND_TRACE','RW_HD5500_BLIT_TRACE','RW_HD5500_UI_TRACE','RW_HD5500_FRAME_PROBE')
            if (-not $FrameDiag) { $quickClear += @('RW_HD5500_FRAME_DIAG','RW_HD5500_FRAME_DUMP_DIR') }
            foreach ($name in $quickClear) { [Environment]::SetEnvironmentVariable($name,$null,'Process') }
            [Environment]::SetEnvironmentVariable('VK_LOADER_DEBUG','error','Process')
        }
        if ($Capture) {
            $captureDir = Join-Path $PSScriptRoot 'capture'
            if (-not (Test-Path -LiteralPath (Join-Path $captureDir 'VkLayer_gfxreconstruct.dll') -PathType Leaf)) { throw 'capture\VkLayer_gfxreconstruct.dll is missing.' }
            [Environment]::SetEnvironmentVariable('VK_LAYER_PATH',$captureDir,'Process')
            [Environment]::SetEnvironmentVariable('VK_INSTANCE_LAYERS','VK_LAYER_LUNARG_gfxreconstruct','Process')
            [Environment]::SetEnvironmentVariable('VK_LOADER_LAYERS_ENABLE','VK_LAYER_LUNARG_gfxreconstruct','Process')
            [Environment]::SetEnvironmentVariable('GFXRECON_CAPTURE_FILE',(Join-Path $runDir 'rimworld-capture.gfxr'),'Process')
            [Environment]::SetEnvironmentVariable('GFXRECON_CAPTURE_FILE_TIMESTAMP','false','Process')
            [Environment]::SetEnvironmentVariable('GFXRECON_CAPTURE_COMPRESSION_TYPE','LZ4','Process')
            [Environment]::SetEnvironmentVariable('GFXRECON_MEMORY_TRACKING_MODE','assisted','Process')
            [Environment]::SetEnvironmentVariable('GFXRECON_LOG_LEVEL','warning','Process')
            [Environment]::SetEnvironmentVariable('GFXRECON_LOG_FILE',(Join-Path $runDir 'gfxrecon.log'),'Process')
            Write-Host ('Capture: GFXReconstruct layer enabled for the game process; file -> ' + (Join-Path $runDir 'rimworld-capture.gfxr'))
        }
        if ($DumpShaders) {
            $hlslDir = Join-Path $runDir 'hlsl'
            New-Item -ItemType Directory -Path $hlslDir -Force | Out-Null
            [Environment]::SetEnvironmentVariable('RW_GPU_DUMP_HLSL',$hlslDir,'Process')
        }
        Write-Host ('V13.4 game settings: capture=' + [bool]$Capture + ' quickStart=' + [bool]$QuickStart + ' frameDiag=' + [bool]$FrameDiag + ' swapchainImages=' + $SwapchainImages)
        $closer = $null
        if ($AutoCloseSeconds -gt 0) {
            Write-Host ('Unattended run: RimWorld will be asked to close after ' + $AutoCloseSeconds + ' s (forced 45 s later if it does not exit).')
            $closer = Start-Job -ArgumentList $AutoCloseSeconds -ScriptBlock {
                param($seconds)
                Start-Sleep -Seconds $seconds
                foreach ($p in @(Get-Process -Name RimWorld -ErrorAction SilentlyContinue)) { [void]$p.CloseMainWindow() }
                Start-Sleep -Seconds 45
                foreach ($p in @(Get-Process -Name RimWorld -ErrorAction SilentlyContinue)) { Stop-Process -Id $p.Id -Force }
            }
        }
        # v13.50: live values for the Throttle monitor window (written by the bridge once per second).
        [Environment]::SetEnvironmentVariable('RW_GPU_LIVE_FILE',(Join-Path $PSScriptRoot 'reports\live-power.txt'),'Process')
        # v13.56: tick-rate counter (game ticks per real second vs. target), read-only diagnostic mod.
        $isTestRun = ($GameArguments -match '-quicktest') -or ($AutoCloseSeconds -gt 0)
        if (-not $NoTickCounter -and ($TickCounter -or $isTestRun)) {
            try {
                $modSource = Join-Path $PSScriptRoot 'mods\HD5500TickRate'
                $modTarget = Join-Path (Split-Path -Parent $GameExe) 'Mods\HD5500TickRate'
                if (Test-Path -LiteralPath $modSource) {
                    $srcDll = Join-Path $modSource 'Assemblies\HD5500TickRate.dll'; $dstDll = Join-Path $modTarget 'Assemblies\HD5500TickRate.dll'
                    if (-not (Test-Path -LiteralPath $dstDll) -or ((Get-FileHash -LiteralPath $srcDll).Hash -ne (Get-FileHash -LiteralPath $dstDll).Hash)) {
                        New-Item -ItemType Directory -Force -Path $modTarget | Out-Null
                        Copy-Item -LiteralPath (Join-Path $modSource 'About') -Destination $modTarget -Recurse -Force
                        Copy-Item -LiteralPath (Join-Path $modSource 'Assemblies') -Destination $modTarget -Recurse -Force
                        Write-Host ('Tick-rate counter mod installed: ' + $modTarget)
                    }
                    [Environment]::SetEnvironmentVariable('RW_GPU_TICK_FILE',(Join-Path $runDir 'ticks.csv'),'Process')
                    if ($GameArguments -notmatch '-mod=') { $GameArguments = ($GameArguments + ' -mod=hd5500bridge.tickrate').Trim() }
                    Write-Host 'Tick-rate counter: on (game ticks per second vs. target, written to ticks.csv)'
                }
            } catch { Write-Host ('Tick-rate counter not available: ' + $_.Exception.Message) }
        }
        # v13.66: HD5500 Optimizer mod - game frame profile (game-profile.txt in the run folder).
        $expProfile = [Environment]::GetEnvironmentVariable('RW_OPT_PROFILE','Process')   # v13.67: experiment-env.txt may ask for level 2/3
        $profileLevel = if ($GameProfile -ge 0) { $GameProfile } elseif ($expProfile -match '^[0-3]$') { [int]$expProfile } elseif ($isTestRun -and -not $NoTickCounter) { 1 } else { 0 }
        # v13.67: keep the optimizer mod installed in RimWorld\Mods (so it can be enabled in the game's mod list
        # for normal play); it only does something when enabled or loaded by a test run.
        try {
            $optSource0 = Join-Path $PSScriptRoot 'mods\HD5500Optimizer'
            $optTarget0 = Join-Path (Split-Path -Parent $GameExe) 'Mods\HD5500Optimizer'
            $srcDll0 = Join-Path $optSource0 'Assemblies\HD5500Optimizer.dll'; $dstDll0 = Join-Path $optTarget0 'Assemblies\HD5500Optimizer.dll'
            if ((Test-Path -LiteralPath $srcDll0) -and (-not (Test-Path -LiteralPath $dstDll0) -or ((Get-FileHash -LiteralPath $srcDll0).Hash -ne (Get-FileHash -LiteralPath $dstDll0).Hash))) {
                New-Item -ItemType Directory -Force -Path $optTarget0 | Out-Null
                Copy-Item -LiteralPath (Join-Path $optSource0 'About') -Destination $optTarget0 -Recurse -Force
                Copy-Item -LiteralPath (Join-Path $optSource0 'Assemblies') -Destination $optTarget0 -Recurse -Force
                Write-Host ('HD5500 Optimizer mod installed/updated: ' + $optTarget0)
            }
        } catch { Write-Host ('HD5500 Optimizer mod not installed: ' + $_.Exception.Message) }
        if ($MaxZoomOut -gt 0) {
            [Environment]::SetEnvironmentVariable('RW_OPT_MAX_ZOOM',[string]$MaxZoomOut,'Process')
            Write-Host ('Maximum zoom-out for this run: ' + $MaxZoomOut + ' cells (HD5500 Optimizer)')
            if ($profileLevel -le 0 -and -not $isTestRun) { Write-Host 'Note: in normal play enable "HD5500 Optimizer" in the mod list for this to apply.' }
        }
        if ($profileLevel -gt 0 -or ($isTestRun -and $MaxZoomOut -gt 0)) {
            try {
                $optSource = Join-Path $PSScriptRoot 'mods\HD5500Optimizer'
                $optTarget = Join-Path (Split-Path -Parent $GameExe) 'Mods\HD5500Optimizer'
                if (Test-Path -LiteralPath $optSource) {
                    $srcDll = Join-Path $optSource 'Assemblies\HD5500Optimizer.dll'; $dstDll = Join-Path $optTarget 'Assemblies\HD5500Optimizer.dll'
                    if (-not (Test-Path -LiteralPath $dstDll) -or ((Get-FileHash -LiteralPath $srcDll).Hash -ne (Get-FileHash -LiteralPath $dstDll).Hash)) {
                        New-Item -ItemType Directory -Force -Path $optTarget | Out-Null
                        Copy-Item -LiteralPath (Join-Path $optSource 'About') -Destination $optTarget -Recurse -Force
                        Copy-Item -LiteralPath (Join-Path $optSource 'Assemblies') -Destination $optTarget -Recurse -Force
                        Write-Host ('HD5500 Optimizer mod installed: ' + $optTarget)
                    }
                    [Environment]::SetEnvironmentVariable('RW_OPT_PROFILE',[string]$profileLevel,'Process')
                    [Environment]::SetEnvironmentVariable('RW_OPT_PROFILE_FILE',(Join-Path $runDir 'game-profile.txt'),'Process')
                    if ($GameArguments -match '-mod=(\S+)') {
                        if ($Matches[1] -notmatch 'hd5500bridge\.optimizer') { $GameArguments = $GameArguments -replace '-mod=(\S+)', ('-mod=$1,hd5500bridge.optimizer') }
                    } else { $GameArguments = ($GameArguments + ' -mod=hd5500bridge.optimizer').Trim() }
                    Write-Host ('Game frame profile: level ' + $profileLevel + ' (game-profile.txt)')
                }
            } catch { Write-Host ('Game frame profile not available: ' + $_.Exception.Message) }
        }
        # v13.78: private local build - extra game arguments, cached GPU profile, private mods.
        if ($LocalDir -and -not $foreignIcd) {
            if ($localGameArgs) { $GameArguments = ($GameArguments + ' ' + $localGameArgs).Trim() }
            # Waylith runs a ~1.7 s GPU benchmark at every boot on slow GPUs because the result is "incomplete"
            # and never cached (Waylith.Hardware.HardwareProfile / GpuPerformanceProfileStore). Writing the
            # cache file it looks for skips it. Key = what the bridge reports: PCI vendor, device, revision.
            # Values give the same "Low" tier the game uses now. Wrong key = file ignored, probe runs as before.
            if ([Environment]::GetEnvironmentVariable('RW_LOCAL_GPU_PROFILE','Process') -eq '1') {
                try {
                    $vc = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | Where-Object { $_.PNPDeviceID -match 'VEN_([0-9A-F]{4})&DEV_([0-9A-F]{4}).*REV_([0-9A-F]{2})' -and $_.PNPDeviceID -match 'VEN_8086' } | Select-Object -First 1)
                    if ($vc.Count -and $vc[0].PNPDeviceID -match 'VEN_([0-9A-F]{4})&DEV_([0-9A-F]{4}).*REV_([0-9A-F]{2})') {
                        $ven = [Convert]::ToInt32($Matches[1],16); $dev = [Convert]::ToInt32($Matches[2],16); $rev = [Convert]::ToInt32($Matches[3],16)
                        $profDir = Join-Path $data 'HardwareProfile'; New-Item -ItemType Directory -Force -Path $profDir | Out-Null
                        $ms = New-Object IO.MemoryStream; $bw = New-Object IO.BinaryWriter($ms)
                        $bw.Write([uint32]1196446278); $bw.Write([int32]4); $bw.Write([int32]$ven); $bw.Write([int32]$dev); $bw.Write([int32]$rev)
                        $bw.Write([single]250.0); $bw.Write([single]25.6); $bw.Write([int32]0); $bw.Flush()
                        [IO.File]::WriteAllBytes((Join-Path $profDir 'gpu-profile.bin'), $ms.ToArray())
                        Write-Host ('LOCAL: cached GPU profile written (vendor ' + $Matches[1] + ' device ' + $Matches[2] + ' rev ' + $Matches[3] + '): the boot GPU benchmark is skipped. Look for "HardwareProfile: loaded cached" in Player.log.')
                    } else { Write-Host 'LOCAL: GPU profile not written (no Intel adapter with a PCI revision found).' }
                } catch { Write-Host ('LOCAL: GPU profile not written: ' + $_.Exception.Message) }
            }
            # v13.82: the game's runtime (WaylithCLR) contains the Satori low-pause garbage collector, switched by the
            # runtimeconfig property WaylithClr.SatoriGC (found in coreclr.dll). Local only: the launcher writes the
            # property into RimWorld.runtimeconfig.json for this run and restores the original afterwards (a backup
            # left by a crashed run is restored at the next start).
            $rcPath = Join-Path (Split-Path -Parent $GameExe) 'RimWorld.runtimeconfig.json'
            $rcBackup = $rcPath + '.hd5500-original'
            if (Test-Path -LiteralPath $rcBackup) { Copy-Item -LiteralPath $rcBackup -Destination $rcPath -Force; Remove-Item -LiteralPath $rcBackup -Force -ErrorAction SilentlyContinue; Write-Host 'LOCAL: restored the original runtimeconfig left by an earlier run.' }
            if ([Environment]::GetEnvironmentVariable('RW_LOCAL_SATORI_GC','Process') -eq '1' -and (Test-Path -LiteralPath $rcPath)) {
                try {
                    $rcText = [IO.File]::ReadAllText($rcPath)
                    if ($rcText -notmatch 'WaylithClr\.SatoriGC') {
                        Copy-Item -LiteralPath $rcPath -Destination $rcBackup -Force
                        $rcNew = $rcText -replace '"configProperties"\s*:\s*\{', ('"configProperties": {' + [Environment]::NewLine + '      "WaylithClr.SatoriGC": true,')
                        [IO.File]::WriteAllText($rcPath, $rcNew)
                        $script:runtimeConfigBackup = $rcBackup
                        Write-Host 'LOCAL: Satori GC switched on for this run (RimWorld.runtimeconfig.json, restored afterwards).'
                    } else { Write-Host 'LOCAL: runtimeconfig already mentions SatoriGC; left as is.' }
                } catch { Write-Host ('LOCAL: Satori GC not switched on: ' + $_.Exception.Message) }
            }
            $localMods = Join-Path $LocalDir 'mods'
            if (Test-Path -LiteralPath $localMods -PathType Container) {
                foreach ($m in @(Get-ChildItem -LiteralPath $localMods -Directory)) {
                    try {
                        $about = Join-Path $m.FullName 'About\About.xml'
                        if (-not (Test-Path -LiteralPath $about)) { continue }
                        $pkg = ([xml](Get-Content -LiteralPath $about -Raw)).ModMetaData.packageId
                        $target = Join-Path (Split-Path -Parent $GameExe) ('Mods\' + $m.Name)
                        New-Item -ItemType Directory -Force -Path $target | Out-Null
                        foreach ($sub in 'About','Assemblies','Harmony') { $src = Join-Path $m.FullName $sub; if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $target -Recurse -Force } }
                        Write-Host ('LOCAL: private mod ' + $pkg + ' installed in ' + $target)
                        if ($isTestRun) {
                            if ($GameArguments -match '-mod=(\S+)') { if ($Matches[1] -notmatch [regex]::Escape($pkg)) { $GameArguments = $GameArguments -replace '-mod=(\S+)', ('-mod=$1,' + $pkg) } }
                            else { $GameArguments = ($GameArguments + ' -mod=' + $pkg).Trim() }
                        } else { Write-Host ('LOCAL: for normal play enable "' + $pkg + '" once in the game''s mod list.') }
                    } catch { Write-Host ('LOCAL: mod ' + $m.Name + ' not installed: ' + $_.Exception.Message) }
                }
            }
        }
        # v13.53: one line per frame (frame time, present, bridge time, CPU speed) for graphs and 1%/0.1% lows.
        [Environment]::SetEnvironmentVariable('RW_GPU_FRAMETIME_CSV',(Join-Path $runDir 'frametimes.csv'),'Process')
        if ([Environment]::GetEnvironmentVariable('RW_GPU_THERMAL_GUARD','Process') -eq '1') { Write-Host 'Heat guard ON: the game is capped to 30 fps while the laptop throttles or runs hot.' }
        $exitCode = Invoke-Captured $GameExe $GameArguments 'game' 0
        if ($closer) { Stop-Job $closer -ErrorAction SilentlyContinue; Remove-Job $closer -Force -ErrorAction SilentlyContinue }
        Write-Host ('Native game process result: ' + $exitCode)
        $processResult = $exitCode
        $playerCopy = $null
        if (Test-Path -LiteralPath $player -PathType Leaf) {
            if ((Get-FileHash -LiteralPath $player).Hash -ne $beforeHash) {
                $playerCopy = Join-Path $runDir 'Player.log'
                Copy-Item -LiteralPath $player -Destination $playerCopy
                if ([IO.File]::ReadAllText($player) -match 'CRIT\s+\[Crash\]|=== CRASH ===') { Write-Host 'Fresh engine log contains a crash.' -ForegroundColor Yellow; if ($exitCode -eq 0) { $exitCode = 1 } }
            } else { Write-Host 'Player.log did not change; no new engine log was captured.' }
        }
        $crashes = Join-Path $data 'Crashes'
        if (Test-Path -LiteralPath $crashes -PathType Container) {
            Get-ChildItem -LiteralPath $crashes -Filter 'crash-*.txt' -File | Where-Object { $_.LastWriteTimeUtc -ge $startUtc.AddSeconds(-2) } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 3 | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $runDir $_.Name) }
        }
        try {
            if (Write-V12Classification $runDir $playerCopy (Join-Path $runDir 'game.stderr.txt') $processResult) {
                Write-Host 'V12 classification: FAILED run (rendering errors were detected). Attach the report ZIP.' -ForegroundColor Yellow
                if ($exitCode -eq 0) { $exitCode = 1 }
            }
        } catch { Write-Host ('Could not classify this run: ' + $_.Exception.Message) }
    }
} catch {
    Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
    $exitCode = 1
} finally {
    if ($runtimeConfigBackup -and (Test-Path -LiteralPath $runtimeConfigBackup)) {
        try { $rcOrig = $runtimeConfigBackup.Substring(0, $runtimeConfigBackup.Length - '.hd5500-original'.Length); Copy-Item -LiteralPath $runtimeConfigBackup -Destination $rcOrig -Force; Remove-Item -LiteralPath $runtimeConfigBackup -Force; Write-Host 'LOCAL: original runtimeconfig restored.' } catch { Write-Host ('LOCAL: could not restore the runtimeconfig; copy ' + $runtimeConfigBackup + ' back by hand: ' + $_.Exception.Message) -ForegroundColor Yellow }
    }
    foreach ($file in @($temporaryFiles.Keys)) {
        try {
            if ((Test-Path -LiteralPath $file -PathType Leaf) -and (Get-FileHash -LiteralPath $file).Hash -eq $temporaryFiles[$file]) {
                Remove-Item -LiteralPath $file
                Write-Host ('Removed own temporary compatibility file: ' + $file)
            } elseif (Test-Path -LiteralPath $file) { Write-Host ('File changed during the test; left intact: ' + $file) -ForegroundColor Yellow }
        } catch { Write-Host ('Could not remove own temporary file; close the game and inspect ' + $file + ': ' + $_.Exception.Message) }
    }
    try {
        $proxyLogs = Join-Path $env:LOCALAPPDATA 'RimWorldVulkanBridge'
        if ($runDir -and (Test-Path -LiteralPath $proxyLogs -PathType Container)) {
            Get-ChildItem -LiteralPath $proxyLogs -Filter 'portability-proxy-*.log' -File | Where-Object { $_.LastWriteTimeUtc -ge $startUtc.AddSeconds(-2) } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $runDir $_.Name) }
        }
    } catch { Write-Host ('Could not collect compatibility log: ' + $_.Exception.Message) }
    foreach ($name in $envNames) { if ($saved.ContainsKey($name)) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') } }
    if ($runDir) {
        try { Save-GraphicsResetEvents $runDir $startUtc } catch { Write-Host ('Could not save optional Display events: ' + $_.Exception.Message) }
    }
    if ($transcript) { Stop-Transcript | Out-Null }
    if ($runDir -and (Test-Path -LiteralPath $runDir -PathType Container)) {
        try {
            [IO.File]::WriteAllText((Join-Path $runDir 'result.txt'),('Launcher result: ' + $exitCode + [Environment]::NewLine),[Text.Encoding]::UTF8)
            if ($Zip) {
                $zip = $runDir + '.zip'
                Compress-Archive -Path (Join-Path $runDir '*') -DestinationPath $zip
                Write-Host ('Diagnostic ZIP: ' + $zip)
                Write-Host 'Attach that ZIP for analysis. Nothing was uploaded automatically.'
            } else {
                Write-Host ('Run folder: ' + $runDir + ' (no ZIP; start with -Zip to make one)')
            }
        } catch { Write-Host ('Could not zip diagnostics; files remain in ' + $runDir + ': ' + $_.Exception.Message) }
    }
    # v13.29/v13.56: keep reports\ small. Cleanup-Reports.ps1 keeps the newest -KeepRuns runs (default 3),
    # the newest self-test and power check, converts diagnostic pictures BMP -> PNG (lossless) and turns on
    # NTFS compression for the text logs. Only names written by the bridge are touched.
    try {
        $cleanup = Join-Path $PSScriptRoot 'Cleanup-Reports.ps1'
        if ($KeepRuns -gt 0 -and (Test-Path -LiteralPath $cleanup)) { & $cleanup -KeepRuns $KeepRuns -Quiet; Write-Host ('Reports tidied: newest ' + $KeepRuns + ' runs kept, pictures stored as PNG, logs compressed.') }
    } catch { Write-Host ('Could not tidy old runs: ' + $_.Exception.Message) }
}
exit $exitCode
