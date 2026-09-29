# push_app.ps1 - build a WASM app, upload it to the device over Wi-Fi, run it, show the output.
# The full dev loop with no cable and no reflash.
#
# Usage:
#   .\sdk\push_app.ps1 -AppDir apps\ciao                    # build + push + run on nucleov2.local
#   .\sdk\push_app.ps1 -AppDir apps\ciao -Device 192.168.0.50
#   .\sdk\push_app.ps1 -AppDir apps\ciao -NoRun             # push only
#   .\sdk\push_app.ps1 -AppDir apps\ciao -Aot               # also build + push app.aot (native)
#
# The device API needs a paired session: run `python tools\pair.py` once (it saves the token to
# %USERPROFILE%\.nucleo\token); -Token or NUCLEO_TOKEN override it.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AppDir,
    [string]$Device = 'nucleov2.local',
    [string]$Token = '',
    [switch]$NoRun,
    [switch]$NoBuild,
    [switch]$Aot
)

$ErrorActionPreference = 'Stop'
$SdkRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $NoBuild) { & (Join-Path $SdkRoot 'build_app.ps1') -AppDir $AppDir -Aot:$Aot }

$manifest = Get-Content (Join-Path $AppDir 'manifest.json') -Raw | ConvertFrom-Json
$appId = $manifest.id
if (-not $appId) { throw "manifest.json has no 'id'" }

$base = "http://$Device"
if (-not $Token) { $Token = $env:NUCLEO_TOKEN }
if (-not $Token) { $tf = Join-Path $env:USERPROFILE '.nucleo\token'; if (Test-Path $tf) { $Token = (Get-Content $tf -Raw).Trim() } }
if (-not $Token) { throw "not paired with the device: run 'python tools\pair.py' first" }
$AuthH = @{ Authorization = "Bearer $Token" }
$curlAuth = @('-H', "Authorization: Bearer $Token")

# Upload via POST /api/fs/write?path=<logical>. The path is LOGICAL (mapped to /sdcard on device),
# and its slashes must stay raw (the device does not URL-decode query values), so no escaping here.
# curl.exe is used instead of Invoke-WebRequest: PS 5.1's IWR -InFile mangles binary POST bodies.
function Push-File([string]$local, [string]$remote) {
    $uri = "$base/api/fs/write?path=$remote"
    $out = & curl.exe -s -S -w '|%{http_code}' @curlAuth -X POST --data-binary "@$local" $uri
    if ($LASTEXITCODE -ne 0) { throw "upload $remote failed (curl exit $LASTEXITCODE)" }
    if ($out -notmatch '\|200$') { throw "upload $remote failed: $out" }
    Write-Host "  pushed $remote"
}

Write-Host "Pushing '$appId' to $Device ..."
Push-File (Join-Path $AppDir 'manifest.json') "/apps/$appId/manifest.json"
Push-File (Join-Path $AppDir 'app.wasm')      "/apps/$appId/app.wasm"
$localAot = Join-Path $AppDir 'app.aot'
if (Test-Path $localAot) {
    Push-File $localAot "/apps/$appId/app.aot"
} else {
    # The device prefers app.aot: a stale one left from an earlier -Aot push would shadow the
    # app.wasm we just uploaded. 404 (nothing there) is the normal case.
    & curl.exe -s -o NUL @curlAuth -X POST "$base/api/fs/delete?path=/apps/$appId/app.aot" | Out-Null
}

if (-not $NoRun) {
    Write-Host "Running '$appId' ..."
    Write-Host "----------------------------------------"
    $uri = "$base/api/app/run?id=$([uri]::EscapeDataString($appId))"
    try {
        $r = Invoke-WebRequest -Uri $uri -Method Post -Headers $AuthH -UseBasicParsing -TimeoutSec 180
        Write-Host $r.Content
    } catch {
        $resp = $_.Exception.Response
        if ($resp) {
            $sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
            Write-Host "ERROR: $($sr.ReadToEnd())"
        } else { throw }
    }
    Write-Host "----------------------------------------"
} else {
    Write-Host "Done. Tile appears after the next boot scan; run now via http://$Device (Run app tab)."
}
