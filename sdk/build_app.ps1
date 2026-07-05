# build_app.ps1 - compile a NucleoOS Anima WASM app from C sources (host ABI v1).
#
# Usage:
#   .\sdk\build_app.ps1 -AppDir apps\ciao            # -> apps\ciao\app.wasm
#   .\sdk\build_app.ps1 -AppDir apps\ciao -Verbose   # show the clang command line
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
    [string]$Clang = ''
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
Write-Host "Deploy: copy manifest.json + app.wasm to /sdcard/apps/$appId/ on the device"
