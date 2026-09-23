# build_app.ps1 - compile a NucleoOS Anima WASM app from C sources (host ABI v1).
#
# Usage:
#   .\sdk\build_app.ps1 -AppDir apps\ciao            # -> apps\ciao\app.wasm
#   .\sdk\build_app.ps1 -AppDir apps\ciao -Verbose   # show the clang command line
#   .\sdk\build_app.ps1 -AppDir apps\ciao -Aot       # also -> apps\ciao\app.aot (native RISC-V)
#
# -Aot compiles app.wasm ahead of time with wamrc for the ESP32-P4 (riscv32, ilp32f). The device
# prefers app.aot when it sits next to app.wasm and falls back to app.wasm if it won't load, so
# always deploy both. wamrc must be built from the SAME WAMR tree as the firmware (the AOT format
# version must match): by default it runs inside WSL from /root/wamrc-build/wamrc (see -Wamrc).
#
# The app directory must contain manifest.json (fields: id, entry, abi, permissions, ...)
# and one or more .c files. Every .c in the directory is compiled together with the SDK
# runtime (sdk\src\nucleo_sdk.c). Output: <AppDir>\app.wasm, ready to copy to
# /sdcard/apps/<id>/ next to its manifest.json.
#
# Toolchain: LLVM clang with the wasm32 backend (winget install LLVM.LLVM). We target the
# WASM MVP feature set (-mcpu=mvp) because the on-device WAMR interpreter is built without
# post-MVP extensions (bulk-memory etc.).
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AppDir,
    [string]$Clang = '',
    [switch]$Aot,
    [string]$Wamrc = '/root/wamrc-build/wamrc',   # path INSIDE the WSL distro
    [string]$WslDistro = 'Ubuntu-24.04'
)

$ErrorActionPreference = 'Stop'
$SdkRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

# --- locate clang -------------------------------------------------------------------------------
if (-not $Clang) {
    $cmd = Get-Command clang -ErrorAction SilentlyContinue
    if ($cmd) { $Clang = $cmd.Source }
    elseif (Test-Path 'C:\Program Files\LLVM\bin\clang.exe') { $Clang = 'C:\Program Files\LLVM\bin\clang.exe' }
    else { throw "clang not found. Install LLVM (winget install LLVM.LLVM) or pass -Clang <path>." }
}

# --- read the manifest --------------------------------------------------------------------------
$manifestPath = Join-Path $AppDir 'manifest.json'
if (-not (Test-Path $manifestPath)) { throw "missing $manifestPath" }
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$appId = $manifest.id
$entry = $manifest.entry
if (-not $appId) { throw "manifest.json has no 'id'" }
if (-not $entry) { $entry = 'run' }

$sources = @(Get-ChildItem (Join-Path $AppDir '*.c') | Select-Object -ExpandProperty FullName)
if ($sources.Count -eq 0) { throw "no .c sources in $AppDir" }
$sources += (Join-Path $SdkRoot 'src\nucleo_sdk.c')

# Absolute paths: [System.IO.File] resolves relative paths against the process directory, not the
# PowerShell location, so a relative AppDir made the checks below read another tree's files.
$AppDir = (Resolve-Path $AppDir).Path
$outWasm = Join-Path $AppDir 'app.wasm'

# --- compile + link -----------------------------------------------------------------------------
$flags = @(
    '--target=wasm32', '-mcpu=mvp', '-O2', '-ffreestanding', '-nostdlib',
    '-fvisibility=hidden', '-Wall', '-Wextra',
    "-I$(Join-Path $SdkRoot 'include')",
    '-Wl,--no-entry', "-Wl,--export=$entry",
    '-Wl,-z,stack-size=8192', '-Wl,--initial-memory=65536',
    '-Wl,--strip-all',
    '-o', $outWasm
) + $sources

Write-Verbose ("clang " + ($flags -join ' '))
& $Clang @flags
if ($LASTEXITCODE -ne 0) { throw "clang failed (exit $LASTEXITCODE)" }

# --- sanity checks ------------------------------------------------------------------------------
$bytes = [System.IO.File]::ReadAllBytes($outWasm)
if ($bytes.Length -lt 8 -or $bytes[0] -ne 0x00 -or $bytes[1] -ne 0x61 -or $bytes[2] -ne 0x73 -or $bytes[3] -ne 0x6d) {
    throw "$outWasm is not a WASM module (bad magic)"
}
if ($bytes.Length -gt 2MB) { throw "$outWasm exceeds the 2MB on-device module cap" }

Write-Host ""
Write-Host "OK  $outWasm  ($($bytes.Length) bytes)  id=$appId entry=$entry"

# --- optional AOT (wamrc in WSL) ----------------------------------------------------------------
$outAot = Join-Path $AppDir 'app.aot'
if ($Aot) {
    # --enable-multi-thread: emits the suspend-flag checks on loop back-edges, so the OS can still
    # kill a runaway app (wasm_runtime_terminate); without it an AOT while(1){} is unstoppable.
    # --disable-bulk-memory after it, --disable-ref-types: the app is built for the MVP, so don't
    # flag post-MVP features it lacks (a runtime without them would reject the image).
    $full = (Resolve-Path $outWasm).Path
    $wslIn = '/mnt/' + $full.Substring(0, 1).ToLower() + ($full.Substring(2) -replace '\\', '/')
    $wslOut = $wslIn -replace '\.wasm$', '.aot'
    $aotArgs = @('--target=riscv32', '--target-abi=ilp32f', '--cpu=generic-rv32',
                 '--cpu-features=+m,+a,+c,+f', '--enable-multi-thread', '--disable-bulk-memory', '--disable-ref-types',
                 '-o', $wslOut, $wslIn)
    Write-Verbose ("wamrc " + ($aotArgs -join ' '))
    & wsl.exe -d $WslDistro -- $Wamrc @aotArgs | Write-Verbose
    if ($LASTEXITCODE -ne 0) { throw "wamrc failed (exit $LASTEXITCODE)" }
    $aotBytes = [System.IO.File]::ReadAllBytes($outAot)
    if ($aotBytes.Length -lt 8 -or $aotBytes[0] -ne 0x00 -or $aotBytes[1] -ne 0x61 -or $aotBytes[2] -ne 0x6f -or $aotBytes[3] -ne 0x74) {
        throw "$outAot is not an AOT image (bad magic)"
    }
    if ($aotBytes.Length -gt 2MB) { throw "$outAot exceeds the 2MB on-device module cap" }
    Write-Host "OK  $outAot  ($($aotBytes.Length) bytes)  riscv32/ilp32f"
    Write-Host "Deploy: copy manifest.json + app.wasm + app.aot to /sdcard/apps/$appId/ on the device"
} else {
    if (Test-Path $outAot) {
        # A stale app.aot would shadow the fresh app.wasm on the device.
        Remove-Item $outAot
        Write-Host "Removed stale $outAot (rebuild with -Aot to regenerate)"
    }
    Write-Host "Deploy: copy manifest.json + app.wasm to /sdcard/apps/$appId/ on the device"
}
