# NucleoV2 OTA release: bump VERSION -> build -> stage bin+manifest -> ensure HTTP server.
# One-shot so a release is a single command instead of ~10 tool calls. See ../SKILL.md.
param(
  [string]$Version = "",   # empty => auto-bump the patch of the current CMake VERSION
  [string]$Notes   = "",
  [switch]$NoServe         # don't (re)start the HTTP server
)
$ErrorActionPreference = 'Stop'

$proj  = 'D:\NucleoV2'
$serve = Join-Path $proj 'ota_serve'
$cmake = Join-Path $proj 'CMakeLists.txt'
$py    = 'C:\Users\indecenti\AppData\Local\Programs\Python\Python312\python.exe'
$port  = 8080
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# 1. current version + resolve target ------------------------------------------------
$txt = [System.IO.File]::ReadAllText($cmake)
if ($txt -notmatch 'project\(nucleos-anima VERSION (\d+)\.(\d+)\.(\d+)\)') {
  throw "VERSION not found in CMakeLists.txt"
}
$cur = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
if (-not $Version) { $Version = "$($Matches[1]).$($Matches[2]).$([int]$Matches[3] + 1)" }
Write-Host "version: $cur -> $Version"

# 2. write the new version (BOM-free) ------------------------------------------------
$new = $txt -replace 'project\(nucleos-anima VERSION \d+\.\d+\.\d+\)', "project(nucleos-anima VERSION $Version)"
[System.IO.File]::WriteAllText($cmake, $new, $utf8NoBom)

# 3. build with ESP-IDF --------------------------------------------------------------
# IDF's export.ps1 + idf.py emit stderr that PS 5.1 wraps as errors; relax Stop so they don't
# abort us, and judge success by the real exit code + a freshly-written bin instead.
$bin = Join-Path $proj 'build\nucleos-anima.bin'
$before = if (Test-Path $bin) { (Get-Item $bin).LastWriteTimeUtc } else { [datetime]::MinValue }
$eap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$env:IDF_TOOLS_PATH = 'D:\esp\tools'
. 'D:\esp\esp-idf-v5.5.2\export.ps1' *> $null
Push-Location $proj
idf.py build *> $null
$code = $LASTEXITCODE
Pop-Location
$ErrorActionPreference = $eap
if ($code -ne 0 -or -not (Test-Path $bin) -or (Get-Item $bin).LastWriteTimeUtc -le $before) {
  [System.IO.File]::WriteAllText($cmake, $txt, $utf8NoBom)   # revert the version bump on failure
  throw "build failed (exit $code); version reverted to $cur. Run 'idf.py build' to see the error."
}

# 4. detect PC Wi-Fi IPv4 for the manifest url ---------------------------------------
$ip = (Get-NetIPAddress -AddressFamily IPv4 |
       Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.InterfaceAlias -like '*Wi-Fi*' } |
       Select-Object -First 1).IPAddress
if (-not $ip) { $ip = '192.168.0.216' }
$origin = 'http://' + $ip + ':' + $port   # concat avoids the ${ip}:${port} interpolation gotcha
if ($ip -ne '192.168.0.216') {
  Write-Host "WARN: PC IP $ip != firmware default 192.168.0.216 - set this URL once in Settings > System update (persists)."
}

# 5. stage bin + BOM-free manifest ---------------------------------------------------
New-Item -ItemType Directory -Force -Path $serve | Out-Null
Copy-Item (Join-Path $proj 'build\nucleos-anima.bin') -Destination $serve -Force
if (-not $Notes) { $Notes = "release $Version" }
$manifest = [ordered]@{
  version = $Version
  url     = "$origin/nucleos-anima.bin"
  notes   = $Notes
} | ConvertTo-Json -Compress
[System.IO.File]::WriteAllText((Join-Path $serve 'manifest.json'), $manifest, $utf8NoBom)

# 6. ensure the HTTP server is up ----------------------------------------------------
$listen = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
if (-not $listen -and -not $NoServe) {
  Start-Process -WindowStyle Hidden -FilePath $py `
    -ArgumentList '-m','http.server',"$port",'--directory',$serve,'--bind','0.0.0.0'
  Start-Sleep -Milliseconds 900
  $listen = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
}

# 7. one-line summary ----------------------------------------------------------------
$binsz = (Get-Item (Join-Path $serve 'nucleos-anima.bin')).Length
$srv = if ($listen) { 'up' } elseif ($NoServe) { 'skipped' } else { 'DOWN' }
Write-Host "PUBLISHED $Version | bin=$binsz | manifest=$origin/manifest.json | server=$srv"
