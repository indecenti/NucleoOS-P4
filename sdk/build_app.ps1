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
#   .\sdk\build_app.ps1 -AppDir apps\wasihello -Wasi # WASI app: standard C library (wasi-libc)
#
# -Wasi builds against wasi-libc (target wasm32-wasip1): stdio, malloc, string/math, time and
# files. printf/puts go to the app's output panel; with the "fs" permission the app's folder
# /sdcard/apps/<id>/data is its "/" (nothing else on the card is reachable). Without a manifest
# "entry" (or with "_start") it is a WASI command: main() runs, exit() ends it. Any other entry
# builds a reactor that exports that function instead. Needs the wasi-sdk sysroot and compiler
# builtins (release assets wasi-sysroot-<v>.tar.gz + libclang_rt-<v>.tar.gz), see -WasiSysroot.
#
#   .\sdk\build_app.ps1 -AppDir apps\w4test -Wasm4   # WASM-4 cart (https://wasm4.org)
#
# -Wasm4 builds a fantasy-console cart with the official WASM-4 C API (sdk\w4\wasm4.h) and the
# upstream template's link flags (imported 64 KB memory, stack first). The manifest needs
# "wasm4": true; the OS then runs it full-screen with a touch gamepad. Already-built carts from
# wasm4.org need no compiling: copy the .wasm as app.wasm next to such a manifest.
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
    [string]$WslDistro = 'Ubuntu-24.04',
    [switch]$Wasi,
    [string]$WasiSysroot = 'D:\esp\wasi-sdk-34\wasi-sysroot-34.0',
    [string]$WasiBuiltins = 'D:\esp\wasi-sdk-34\libclang_rt-34.0\wasm32-unknown-wasip1\libclang_rt.builtins.a',
    [int]$WasiStackKb = 64,                       # C stack inside linear memory (not manifest stack_kb)
    [switch]$Wasm4,
    [string]$W4Run = '/root/w4harness/w4run'      # PC harness (tools/w4harness), for -Wasm4 -Aot
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
if ($Wasi -and $Wasm4) { throw "-Wasi and -Wasm4 are exclusive" }
if ($Wasm4 -and $manifest.wasm4 -ne $true) { throw "a -Wasm4 cart needs `"wasm4`": true in manifest.json" }
$reactor = $Wasi -and $entry -and $entry -ne '_start'
if ($Wasm4) { $entry = 'update' }
if (-not $entry) { $entry = if ($Wasi) { '_start' } else { 'run' } }

$sources = @(Get-ChildItem (Join-Path $AppDir '*.c') | Select-Object -ExpandProperty FullName)
if ($sources.Count -eq 0) { throw "no .c sources in $AppDir" }
# The freestanding runtime defines memcpy/strlen itself; under WASI those come from wasi-libc.
# A WASM-4 cart links nothing of ours: it talks only to the console API (sdk\w4\wasm4.h).
if (-not $Wasm4) {
    $sources += (Join-Path $SdkRoot $(if ($Wasi) { 'src\nucleo_sdk_wasi.c' } else { 'src\nucleo_sdk.c' }))
}

# Absolute paths: [System.IO.File] resolves relative paths against the process directory, not the
# PowerShell location, so a relative AppDir made the checks below read another tree's files.
$AppDir = (Resolve-Path $AppDir).Path
$outWasm = Join-Path $AppDir 'app.wasm'

# --- compile + link -----------------------------------------------------------------------------
if (($Wasi -or $Wasm4) -and -not (Test-Path (Join-Path $WasiSysroot 'lib\wasm32-wasip1\libc.a'))) {
    throw "WASI sysroot not found at $WasiSysroot (wasi-sdk release asset wasi-sysroot-<v>.tar.gz)"
}
if (($Wasi -or $Wasm4) -and -not (Test-Path $WasiBuiltins)) {
    throw "compiler builtins not found at $WasiBuiltins (wasi-sdk release asset libclang_rt-<v>.tar.gz)"
}
if ($Wasm4) {
    # The official WASM-4 C template (cli/assets/templates/c/Makefile): a wasi-sdk reactor whose
    # 64 KB memory is imported from the console, stack placed first (below the 0x19a0 program
    # area), no entry. wasi-libc only supplies memset/memcpy & co.
    $flags = @(
        '--target=wasm32-wasip1', "--sysroot=$WasiSysroot", '-Oz', '-Wall', '-Wextra',
        "-I$(Join-Path $SdkRoot 'w4')", '-nodefaultlibs', '-mexec-model=reactor',
        '-Wl,-zstack-size=14752,--no-entry,--import-memory,--initial-memory=65536,--max-memory=65536,--stack-first',
        '-Wl,--strip-all,--gc-sections', '-o', $outWasm
    ) + $sources + @('-lc', $WasiBuiltins)
} elseif ($Wasi) {
    # -nodefaultlibs + explicit libc/builtins: the system clang has no wasm32 builtins in its own
    # resource dir. No -mcpu=mvp: wasi-libc itself is built with bulk-memory/reference-types, which
    # the firmware's WAMR supports (CONFIG_WAMR_ENABLE_REF_TYPES).
    $flags = @(
        '--target=wasm32-wasip1', "--sysroot=$WasiSysroot", '-O2', '-Wall', '-Wextra',
        "-I$(Join-Path $SdkRoot 'include')", '-nodefaultlibs'
    )
    if ($reactor) { $flags += @('-mexec-model=reactor', "-Wl,--export=$entry") }
    $flags += @("-Wl,-z,stack-size=$($WasiStackKb * 1024)", '-Wl,--strip-all', '-o', $outWasm) +
              $sources + @('-lc', $WasiBuiltins)
} else {
    $flags = @(
        '--target=wasm32', '-mcpu=mvp', '-O2', '-ffreestanding', '-nostdlib',
        '-fvisibility=hidden', '-Wall', '-Wextra',
        "-I$(Join-Path $SdkRoot 'include')",
        '-Wl,--no-entry', "-Wl,--export=$entry",
        '-Wl,-z,stack-size=8192', '-Wl,--initial-memory=65536',
        '-Wl,--strip-all',
        '-o', $outWasm
    ) + $sources
}

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
    # flag post-MVP features it lacks (a runtime without them would reject the image). A -Wasi app
    # does use them (wasi-libc), so it keeps both.
    $full = (Resolve-Path $outWasm).Path
    $wslIn = '/mnt/' + $full.Substring(0, 1).ToLower() + ($full.Substring(2) -replace '\\', '/')
    $wslOut = $wslIn -replace '\.wasm$', '.aot'
    $prep = $null
    if ($Wasm4) {
        # wamrc bakes WAMR's load-time memory shrink into the image, and a cart owns all 64 KB:
        # compile the bytes the device would load (nv_w4_prepare_module, via the PC harness).
        $prep = [System.IO.Path]::GetTempFileName()
        $wslPrep = '/mnt/' + $prep.Substring(0, 1).ToLower() + ($prep.Substring(2) -replace '\\', '/')
        & wsl.exe -d $WslDistro -- $W4Run $wslIn --prep $wslPrep | Write-Verbose
        if ($LASTEXITCODE -ne 0) {
            Remove-Item -ErrorAction SilentlyContinue $prep
            throw "w4run --prep failed: build the PC harness first (bash tools/w4harness/build.sh in WSL)"
        }
        $wslIn = $wslPrep
    }
    $aotArgs = @('--target=riscv32', '--target-abi=ilp32f', '--cpu=generic-rv32',
                 '--cpu-features=+m,+a,+c,+f', '--enable-multi-thread')
    if (-not $Wasi -and -not $Wasm4) { $aotArgs += @('--disable-bulk-memory', '--disable-ref-types') }
    $aotArgs += @('-o', $wslOut, $wslIn)
    Write-Verbose ("wamrc " + ($aotArgs -join ' '))
    & wsl.exe -d $WslDistro -- $Wamrc @aotArgs | Write-Verbose
    $rc = $LASTEXITCODE
    if ($prep) { Remove-Item -ErrorAction SilentlyContinue $prep }
    if ($rc -ne 0) { throw "wamrc failed (exit $rc)" }
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
