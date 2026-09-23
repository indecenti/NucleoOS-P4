# w4_import.ps1 - turn an existing WASM-4 cart (.wasm, any source language) into a NucleoOS app.
#
# Usage:
#   .\sdk\w4_import.ps1 -Cart D:\carts\snake.wasm                    # -> apps\snake\
#   .\sdk\w4_import.ps1 -Cart snake.wasm -Id snake -Name "Snake"
#   .\sdk\w4_import.ps1 -Cart snake.wasm -Push -Device 192.168.0.128  # also upload to the board
#
# The app folder gets:
#   manifest.json  "wasm4": true - all the OS needs to run it full-screen with the touch gamepad
#   app.wasm       the cart, unchanged
#   app.aot        native RISC-V (default; -NoAot skips it). Many carts are too heavy for the P4's
#                  interpreter, so AOT matters. The bytes are first prepared exactly like the
#                  device prepares app.wasm (tools/w4harness `w4run --prep`), then compiled by wamrc.
#   icon.argb      80x80 icon of the cart's own screen after a few seconds of autoplay (-NoIcon).
#                  Note: the launcher currently draws compiled icons only (the SD icon loader was
#                  removed after 1.1.57), so carts show the generic game tile for now.
# AOT and icon need the PC harness (WSL): bash tools/w4harness/build.sh. Without it the import
# still works, with app.wasm only and the generic tile. Carts are other people's work: keep their
# license with them.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Cart,
    [string]$Id = '',
    [string]$Name = '',
    [string]$OutRoot = '',
    [switch]$NoAot,
    [switch]$NoIcon,
    [switch]$Push,
    [string]$Device = 'nucleov2.local',
    [string]$W4Run = '/root/w4harness/w4run',     # path INSIDE the WSL distro
    [string]$Wamrc = '/root/wamrc-build/wamrc',
    [string]$WslDistro = 'Ubuntu-24.04'
)

$ErrorActionPreference = 'Stop'
$SdkRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutRoot) { $OutRoot = Join-Path (Split-Path -Parent $SdkRoot) 'apps' }

function ConvertTo-WslPath([string]$p) {
    $full = [System.IO.Path]::GetFullPath($p)
    return '/mnt/' + $full.Substring(0, 1).ToLower() + ($full.Substring(2) -replace '\\', '/')
}
function Test-Wsl([string]$path) {
    & wsl.exe -d $WslDistro -- test -x $path 2>$null
    return $LASTEXITCODE -eq 0
}

$cartPath = (Resolve-Path $Cart).Path
$bytes = [System.IO.File]::ReadAllBytes($cartPath)
if ($bytes.Length -lt 8 -or $bytes[0] -ne 0 -or $bytes[1] -ne 0x61 -or $bytes[2] -ne 0x73 -or $bytes[3] -ne 0x6d) {
    throw "$Cart is not a WASM module"
}
if ($bytes.Length -gt 2MB) { throw "$Cart exceeds the 2 MB module cap" }

# id: [A-Za-z0-9_-], at most 31 chars (the OS rejects anything else)
if (-not $Id) { $Id = [System.IO.Path]::GetFileNameWithoutExtension($cartPath) }
$Id = ($Id -replace '[^A-Za-z0-9_-]', '-').Trim('-')
if ($Id.Length -gt 31) { $Id = $Id.Substring(0, 31) }
if (-not $Id) { throw "cannot derive an app id from '$Cart' (pass -Id)" }
if (-not $Name) { $Name = (Get-Culture).TextInfo.ToTitleCase(($Id -replace '[-_]+', ' ')) }
if ($Name.Length -gt 39) { $Name = $Name.Substring(0, 39) }

$dir = Join-Path $OutRoot $Id
New-Item -ItemType Directory -Force $dir | Out-Null
$wasm = Join-Path $dir 'app.wasm'
Copy-Item -Force $cartPath $wasm
$manifest = [ordered]@{ id = $Id; name = $Name; version = '1.0'; wasm4 = $true } | ConvertTo-Json
[System.IO.File]::WriteAllText((Join-Path $dir 'manifest.json'), $manifest, (New-Object System.Text.UTF8Encoding($false)))

$outAot = Join-Path $dir 'app.aot'
$outIcon = Join-Path $dir 'icon.argb'
$haveRun = Test-Wsl $W4Run
$notes = @()
if (-not $haveRun -and (-not $NoAot -or -not $NoIcon)) {
    $notes += "PC harness not built (bash tools/w4harness/build.sh in WSL): no app.aot, no icon"
}

# Try the cart on the PC first: a cart that can't even start here won't on the board either.
if ($haveRun) {
    $check = & wsl.exe -d $WslDistro -- $W4Run (ConvertTo-WslPath $wasm) --frames 240 --quiet `
        $(if (-not $NoIcon) { '--icon'; (ConvertTo-WslPath $outIcon) })
    Write-Verbose "w4run: $check"
    if ($check -notmatch ' OK ') { $notes += "PC test: $check" }
    if ($NoIcon -and (Test-Path $outIcon)) { Remove-Item $outIcon }
}

if ($haveRun -and -not $NoAot) {
    $prep = [System.IO.Path]::GetTempFileName()
    try {
        & wsl.exe -d $WslDistro -- $W4Run (ConvertTo-WslPath $wasm) --prep (ConvertTo-WslPath $prep) | Write-Verbose
        if ($LASTEXITCODE -ne 0) { throw "w4run --prep failed" }
        & wsl.exe -d $WslDistro -- $Wamrc --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 `
            --cpu-features=+m,+a,+c,+f --enable-multi-thread -o (ConvertTo-WslPath $outAot) (ConvertTo-WslPath $prep) | Write-Verbose
        if ($LASTEXITCODE -ne 0) { throw "wamrc failed (exit $LASTEXITCODE)" }
    } finally {
        Remove-Item -ErrorAction SilentlyContinue $prep
    }
} elseif (Test-Path $outAot) {
    Remove-Item $outAot   # a stale app.aot would shadow the new cart on the device
}

$extra = @()
if (Test-Path $outAot) { $extra += 'app.aot' }
if (Test-Path $outIcon) { $extra += 'icon' }
Write-Host "OK  $dir  id=$Id name=`"$Name`"  ($($bytes.Length) bytes$(if ($extra) { ', + ' + ($extra -join ', ') }))"
foreach ($n in $notes) { Write-Host "    note: $n" }
if ($Push) {
    & (Join-Path $SdkRoot 'push_app.ps1') -AppDir $dir -Device $Device -NoBuild -NoRun
    if (Test-Path $outIcon) {
        & curl.exe -s -S -o NUL -X POST --data-binary "@$outIcon" "http://$Device/api/fs/write?path=/apps/$Id/icon.argb"
    }
}
