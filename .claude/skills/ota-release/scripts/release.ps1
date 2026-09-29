# NucleoV2 OTA release: bump VERSION -> build -> publish (GitHub Pages and/or the local server).
# One-shot so a release is a single command instead of ~10 tool calls. See ../SKILL.md.
param(
  [string]$Version = "",   # empty => auto-bump the patch of the current CMake VERSION
  [string]$Notes   = "",
  # github (default): release + ota/manifest.json on indecenti/nucleoos-p4-store (tools/dist.py),
  #         where firmware >= 1.1.108 looks. local: ota_serve/ on :8080, for a board pointed at
  #         the PC (tests, or one still older than 1.1.108). both: the two.
  [ValidateSet('github','local','both')][string]$Target = 'github',
  # the tree to bump + build: the shared checkout, or an isolated worktree (peer WIP left out).
  # tools/dist.py and ota_serve/ always come from the shared checkout.
  [string]$Proj = 'D:\NucleoV2',
  [switch]$NoServe         # local: don't (re)start the HTTP server
)
$ErrorActionPreference = 'Stop'

$repo  = 'D:\NucleoV2'
$proj  = $Proj
$serve = Join-Path $repo 'ota_serve'
$cmake = Join-Path $proj 'CMakeLists.txt'
$py    = 'C:\Users\<user>\AppData\Local\Programs\Python\Python312\python.exe'
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

if (-not $Notes) { $Notes = "release $Version" }

# 4. GitHub: release asset + ota/manifest.json, waits until Pages serves it ---------------
if ($Target -ne 'local') {
  $ErrorActionPreference = 'Continue'   # gh/git progress on stderr is not an error
  & $py (Join-Path $repo 'tools\dist.py') firmware --version $Version --notes $Notes --bin $bin
  $code = $LASTEXITCODE
  $ErrorActionPreference = $eap
  if ($code -ne 0) {
    throw "GitHub publish failed (exit $code). The build is done: retry with 'python tools/dist.py firmware --version $Version --notes ""...""'."
  }
}
if ($Target -eq 'github') { return }

# 5. local: detect PC Wi-Fi IPv4 for the manifest url ---------------------------------
$ip = (Get-NetIPAddress -AddressFamily IPv4 |
       Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.InterfaceAlias -like '*Wi-Fi*' } |
       Select-Object -First 1).IPAddress
if (-not $ip) { $ip = '192.168.1.40' }
$origin = 'http://' + $ip + ':' + $port   # concat avoids the ${ip}:${port} interpolation gotcha
if ($ip -ne '192.168.1.40') {
  Write-Host "WARN: PC IP $ip != 192.168.1.40 - a board pointed at the PC needs this URL in Settings > System update."
}

# 6. stage bin + BOM-free manifest ---------------------------------------------------
New-Item -ItemType Directory -Force -Path $serve | Out-Null
Copy-Item (Join-Path $proj 'build\nucleos-anima.bin') -Destination $serve -Force
# Signed like the GitHub one (tools/ota_sign.py): firmware from 1.1.123 on refuses unsigned updates.
& $py (Join-Path $repo 'tools\ota_sign.py') manifest --bin (Join-Path $serve 'nucleos-anima.bin') `
  --url "$origin/nucleos-anima.bin" --notes $Notes --out (Join-Path $serve 'manifest.json')
if ($LASTEXITCODE -ne 0) { throw "signing the local manifest failed (release key missing?)" }

# 7. ensure the HTTP server is up ----------------------------------------------------
$listen = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
if (-not $listen -and -not $NoServe) {
  Start-Process -WindowStyle Hidden -FilePath $py `
    -ArgumentList '-m','http.server',"$port",'--directory',$serve,'--bind','0.0.0.0'
  Start-Sleep -Milliseconds 900
  $listen = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
}

# 8. one-line summary ----------------------------------------------------------------
$binsz = (Get-Item (Join-Path $serve 'nucleos-anima.bin')).Length
$srv = if ($listen) { 'up' } elseif ($NoServe) { 'skipped' } else { 'DOWN' }
Write-Host "PUBLISHED-LOCAL $Version | bin=$binsz | manifest=$origin/manifest.json | server=$srv"
