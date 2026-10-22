# v13.76: "does this PC need the bridge?" - read-only graphics compatibility check for RimWorld 1.7.
# Used by the Bridge menu (Home page, one line) and by the PC check (full section). Changes nothing.
#
# RimWorld 1.7 (Waylith engine) renders only through Vulkan. Three groups of PCs:
#   * GPU with a Vulkan driver          -> the game runs natively; the bridge is not needed (driver
#                                          updates are the usual fix when it crashes).
#   * Direct3D 11 GPU without Vulkan    -> this is what the bridge is for (Intel HD 2500-6000 /
#                                          Iris before Skylake, NVIDIA GeForce 400/500 (Fermi),
#                                          AMD Radeon HD 5000/6000 (TeraScale 2)).
#   * older than Direct3D 11            -> cannot run 1.7 (Intel HD 2000/3000, GMA, GeForce 9/200/300,
#                                          Radeon HD 2000-4000).
# Name patterns are a best effort; the bridge's own [RW-GPU-COMPAT] log line (feature level) is
# the final word for bridge users.

function Get-GpuKind([string]$name) {
        $kind = 'unknown'; $advice = ''
    if ($name -match 'Microsoft Basic Display|Microsoft Basic Render') {
        $kind = 'no-driver'; $advice = 'No graphics driver is installed (Windows basic driver). Install the driver from the PC or GPU maker first.'
    }
    elseif ($name -match 'Intel') {
        if ($name -match 'HD Graphics (2000|3000)\b|GMA|G41|G45|Q45|X4500') { $kind = 'too-old'; $advice = 'Too old for RimWorld 1.7 (no Direct3D 11). The bridge cannot help.' }
        elseif ($name -match 'HD Graphics (2500|4000|4200|4400|4600|5000|5300|5500|5600|6000|P4000|P4600|P4700|P5700|P6300)\b|Iris(\(R\)|\(TM\))?( Pro)?( Graphics)? (5100|5200|6100|6200|P6300)\b|Intel\(R\) HD Graphics$') { $kind = 'bridge'; $advice = 'No Vulkan driver exists for this Intel GPU (Ivy Bridge to Broadwell). This is what the bridge is for: use Play RimWorld.' }
        elseif ($name -match 'UHD|Iris\(R\) Xe|Iris Xe|Iris\(R\) Plus|Iris Plus|Arc|HD Graphics (5\d\d|6\d\d|P5\d\d|P6\d\d)\b') { $kind = 'native'; $advice = 'This Intel GPU has a Vulkan driver: RimWorld 1.7 should run without the bridge. If it crashes: install the newest Intel driver (Gen 6-10 laptops: Intel "legacy" graphics driver 31.0.101.2141 or newer, not only the PC maker''s old driver).' }
    }
    elseif ($name -match 'NVIDIA|GeForce|Quadro') {
        if ($name -match 'GeForce (8|9)\d{3}|GeForce G?T?S? ?(1|2|3)\d{2}M?\b|GeForce 9\d{3}') { $kind = 'too-old'; $advice = 'Too old for RimWorld 1.7 (no Direct3D 11). The bridge cannot help.' }
        elseif ($name -match 'GeForce (GT|GTS|GTX)? ?(4|5)\d{2}M?\b|Quadro (2000|4000|5000|6000|600)\b') { $kind = 'bridge'; $advice = 'NVIDIA never released Vulkan for this GeForce 400/500 (Fermi) GPU: the bridge should work (untested - please report).' }
        else { $kind = 'native'; $advice = 'This NVIDIA GPU has Vulkan: RimWorld 1.7 should run without the bridge. GTX 600/700 (Kepler) only get security drivers (473/474.xx) - install the newest one.' }
    }
    elseif ($name -match 'AMD|ATI|Radeon') {
        if ($name -match 'Radeon HD [2-4]\d{3}\b|Radeon X\d') { $kind = 'too-old'; $advice = 'Too old for RimWorld 1.7 (no Direct3D 11). The bridge cannot help.' }
        elseif ($name -match 'Radeon HD [56]\d{3}\b|Radeon HD 7[3-6]\d{2}\b') { $kind = 'bridge'; $advice = 'This Radeon (TeraScale) has no Vulkan driver: the bridge should work (untested - please report).' }
        else { $kind = 'native'; $advice = 'This Radeon has Vulkan: RimWorld 1.7 should run without the bridge. Radeon HD 7000/R7/R9 200-300 (GCN 1-3): AMD "Legacy" driver 22.6.1 is the last one - older OEM drivers are known to show wrong sprite sizes. Newer Radeons (e.g. RX 9070 XT, driver 26.10.1) draw items as only their top-left corner in 1.7.4984: community fix RimWorldAMDFix (github.com/RevenantBob/RimWorldAMDFix, a Vulkan layer that redoes one BC7 atlas copy).' }
    }
    [pscustomobject]@{ Kind = $kind; Advice = $advice }
}

function Get-GpuCompat {
    $result = @()
    $vkDrivers = @()
    foreach ($k in 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers', 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers') {
        try { $p = Get-ItemProperty -Path $k -ErrorAction Stop; $vkDrivers += @($p.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { $_.Name }) } catch { }
    }
    # GPU drivers register their Vulkan ICD per adapter as well (Intel/AMD/NVIDIA modern drivers)
    try {
        Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' -ErrorAction Stop | ForEach-Object {
            try {
                $q = Get-ItemProperty -Path $_.PSPath -ErrorAction Stop
                foreach ($n in 'VulkanDriverName', 'VulkanDriverNameWoW') { if ($q.PSObject.Properties[$n]) { $vkDrivers += @($q.$n) } }
            } catch { }
        }
    } catch { }
    $vkDrivers = @($vkDrivers | Where-Object { $_ -and $_ -notmatch 'HD5500-GPU-Bridge|ashes' } | Sort-Object -Unique)
    $gpus = @()
    try { $gpus = @(Get-CimInstance Win32_VideoController -ErrorAction Stop) } catch { }
    foreach ($g in $gpus) {
        $name = [string]$g.Name
        $k = Get-GpuKind $name; $kind = $k.Kind; $advice = $k.Advice
        $vramMb = if ($g.AdapterRAM) { [Math]::Round([double][uint32]$g.AdapterRAM / 1MB) } else { 0 }
        $result += [pscustomobject]@{ Name = $name; Kind = $kind; Advice = $advice; Driver = [string]$g.DriverVersion; VramMb = $vramMb }
    }
    $layers = @()
    foreach ($k in 'HKLM:\SOFTWARE\Khronos\Vulkan\ImplicitLayers', 'HKCU:\SOFTWARE\Khronos\Vulkan\ImplicitLayers', 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\ImplicitLayers') {
        try { $p = Get-ItemProperty -Path $k -ErrorAction Stop; $layers += @($p.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.Name) }) } catch { }
    }
    [pscustomobject]@{ Gpus = $result; VulkanDrivers = $vkDrivers; ImplicitLayers = @($layers | Sort-Object -Unique) }
}

# One plain-language line for the menu's Home page.
function Get-GpuCompatLine {
    try {
        $c = Get-GpuCompat
        $native = @($c.Gpus | Where-Object { $_.Kind -eq 'native' })
        $bridge = @($c.Gpus | Where-Object { $_.Kind -eq 'bridge' })
        if ($native.Count -and $c.VulkanDrivers.Count) { return ('Note: ' + $native[0].Name + ' has a Vulkan driver - RimWorld 1.7 may run fine without the bridge (and faster). See Laptop health > PC check.') }
        if ($bridge.Count) { return ('Graphics: ' + $bridge[0].Name + ' - no Vulkan driver, the bridge is the right tool for this PC.') }
        if (@($c.Gpus | Where-Object { $_.Kind -eq 'too-old' }).Count) { return 'Graphics: this GPU is older than Direct3D 11 - RimWorld 1.7 cannot run on it, also not with the bridge.' }
        return ''
    } catch { return '' }
}
