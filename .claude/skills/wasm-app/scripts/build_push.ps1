# build_push.ps1 — build a NucleoOS WASM app and push it to the board over Wi-Fi, robustly.
# Wraps sdk\build_app.ps1 + sdk\push, but adds what the plain push lacks: host DISCOVERY (mDNS
# nucleov2.local drops constantly on this board — fall back to the last-known IP with retries) and
# curl --data-binary for every file (python urllib TRUNCATES multi-MB uploads — never use it).
#
#   .\.claude\skills\wasm-app\scripts\build_push.ps1 -AppDir apps\myapp
#   .\.claude\skills\wasm-app\scripts\build_push.ps1 -AppDir apps\myapp -Assets   # also push img\*.565 + snd\*.wav
#   .\.claude\skills\wasm-app\scripts\build_push.ps1 -AppDir apps\myapp -Device 192.168.0.128
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppDir,
    [switch]$Assets,
    [switch]$NoBuild,
    [string]$Device = '',
    [string[]]$Hosts = @('192.168.0.128','nucleov2.local')   # try IP first (mDNS is flaky), then name
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path   # ...\.claude\skills\wasm-app\scripts -> repo root
$manifest = Get-Content (Join-Path $AppDir 'manifest.json') -Raw | ConvertFrom-Json
$id = $manifest.id
if (-not $id) { throw "manifest.json has no 'id'" }

if (-not $NoBuild) { & (Join-Path $root 'sdk\build_app.ps1') -AppDir $AppDir; if ($LASTEXITCODE) { throw "build failed" } }

# --- discover a reachable board (retry — the Wi-Fi/mDNS drops for ~seconds at a time) ---
function Find-Board {
    param($cands)
    foreach ($try in 1..8) {
        foreach ($h in $cands) { if (& curl.exe -s --max-time 5 "http://$h/api/info" 2>$null) { return $h } }
        Start-Sleep -Seconds 4
    }
    return $null
}
$cands = if ($Device) { @($Device) } else { $Hosts }
$dev = Find-Board $cands
if (-not $dev) { throw "board unreachable (tried $($cands -join ', ')). Wake it / check Wi-Fi and retry." }
Write-Host "board: $dev  ($(& curl.exe -s "http://$dev/api/info"))"

function Push-File($local, $remote) {
    if (-not (Test-Path $local)) { return }
    $out = & curl.exe -s -S --max-time 180 -w '|%{http_code}' -X POST --data-binary "@$local" "http://$dev/api/fs/write?path=$remote"
    if ($out -notmatch '\|200$') { throw "upload $remote failed: $out" }
    Write-Host "  pushed $remote"
}

Push-File (Join-Path $AppDir 'app.wasm')      "/apps/$id/app.wasm"
Push-File (Join-Path $AppDir 'manifest.json') "/apps/$id/manifest.json"
if ($Assets) {
    Get-ChildItem (Join-Path $AppDir 'img\*.565') -ErrorAction SilentlyContinue | ForEach-Object { Push-File $_.FullName "/apps/$id/img/$($_.Name)" }
    Get-ChildItem (Join-Path $AppDir 'snd\*.wav') -ErrorAction SilentlyContinue | ForEach-Object { Push-File $_.FullName "/apps/$id/snd/$($_.Name)" }
}
Write-Host "OK - reopen '$id' on the device to load the new build (a running instance keeps the old one)."
