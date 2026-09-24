# Ricompila ..\mpeg1.wasm (decoder MPEG-1/MP2 del Video Player web) da mpeg1_wasm.c + pl_mpeg.h.
# Equivalente PowerShell di build_mpeg1_wasm.sh (stessi flag, stessi percorsi predefiniti).
#
#   .\sd\web\apps\video-player\wasm\build_mpeg1_wasm.ps1
#   .\sd\web\apps\video-player\wasm\build_mpeg1_wasm.ps1 -Clang 'C:\LLVM\bin\clang.exe'
#
# Serve clang con backend wasm32 + wasm-ld (winget install LLVM.LLVM) e il sysroot + builtins di
# wasi-sdk (asset di release wasi-sysroot-<v>.tar.gz e libclang_rt-<v>.tar.gz). Rigenera anche i
# gemelli .gz (il server web della board li preferisce ai file originali).
param(
    [string]$Clang = '',
    [string]$WasiSysroot = 'D:\esp\wasi-sdk-34\wasi-sysroot-34.0',
    [string]$WasiBuiltins = 'D:\esp\wasi-sdk-34\libclang_rt-34.0\wasm32-unknown-wasip1\libclang_rt.builtins.a'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path (Split-Path $here -Parent) 'mpeg1.wasm'

if (-not $Clang) {
    $cmd = Get-Command clang -ErrorAction SilentlyContinue
    if ($cmd) { $Clang = $cmd.Source }
    elseif (Test-Path 'C:\Program Files\LLVM\bin\clang.exe') { $Clang = 'C:\Program Files\LLVM\bin\clang.exe' }
    else { throw 'clang non trovato: installa LLVM (winget install LLVM.LLVM) o passa -Clang' }
}
if (-not (Test-Path (Join-Path $WasiSysroot 'lib\wasm32-wasip1\libc.a'))) { throw "sysroot WASI mancante: $WasiSysroot" }
if (-not (Test-Path $WasiBuiltins)) { throw "builtins mancanti: $WasiBuiltins" }

$flags = @(
    '--target=wasm32-wasip1', "--sysroot=$WasiSysroot",
    '-O3', '-flto', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
    '-nodefaultlibs', '-mexec-model=reactor',
    '-Wl,--strip-all', '-Wl,--gc-sections', '-Wl,--lto-O3',
    '-Wl,-z,stack-size=262144', '-Wl,--initial-memory=16777216', '-Wl,--max-memory=1073741824',
    '-o', $out, (Join-Path $here 'mpeg1_wasm.c'), '-lc', $WasiBuiltins
)
& $Clang @flags
if ($LASTEXITCODE -ne 0) { throw "clang fallito (exit $LASTEXITCODE)" }

# gemelli .gz (gzip -9, senza nome né data: stesso formato degli altri file del web OS)
function Write-Gz([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $fs = [System.IO.File]::Create("$path.gz")
    try {
        $gz = New-Object System.IO.Compression.GZipStream($fs, [System.IO.Compression.CompressionLevel]::Optimal)
        try { $gz.Write($bytes, 0, $bytes.Length) } finally { $gz.Dispose() }
    } finally { $fs.Dispose() }
}
foreach ($f in @($out, (Join-Path $here 'mpeg1_wasm.c'), (Join-Path $here 'pl_mpeg.h'), (Join-Path $here 'build_mpeg1_wasm.sh'), $PSCommandPath)) { Write-Gz $f }
Get-Item $out, "$out.gz" | Format-Table Name, Length
