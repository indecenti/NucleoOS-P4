# shot.ps1 — drive the NucleoV2 UI over the web API and capture the screen.
# One call can open an app / tap a point / go home, wait for the UI to settle, then save a JPEG
# of the panel. Backed by the on-device /api/ui/* + /api/screen endpoints (nv_web + nv_ui).
#
# Examples:
#   shot.ps1 -Open sysmon -Out perf.jpg          # open System Monitor, screenshot it
#   shot.ps1 -Tap "20,150" -Out proc.jpg         # tap (rail/tab/button) then screenshot
#   shot.ps1 -GoHome                             # back to launcher (+ screenshot)
#   shot.ps1 -State                              # print the foreground app id, no capture
#   shot.ps1 -Out now.jpg                        # just capture the current screen
param(
    [string]$Ip   = $(if ($env:NV_BOARD_IP) { $env:NV_BOARD_IP } else { "192.168.0.128" }),
    [string]$Open,                 # native app id to open (settings/files/sysmon/...)
    [string]$Tap,                  # synthetic pointer tap, "x,y" (0..1023 , 0..599)
    [switch]$GoHome,               # return to the launcher first
    [switch]$State,                # print current app id as JSON and exit (no capture)
    [string]$Out,                  # save the screenshot here (JPEG). Defaults to a temp file
    [int]$Wait    = 800            # ms to wait before capture (let the UI settle / animate)
)
$ErrorActionPreference = "Stop"
$base = "http://$Ip"

function Api($path) {
    try { return Invoke-RestMethod -Uri "$base$path" -TimeoutSec 8 }
    catch { Write-Error "API $path failed: $($_.Exception.Message)"; exit 1 }
}

if ($State) { (Api "/api/ui/state") | ConvertTo-Json -Compress; exit 0 }

if ($GoHome)  { Api "/api/ui/home" | Out-Null }

if ($Open)  {
    $r = Api "/api/ui/open?id=$Open"
    if (-not $r.ok) { Write-Error "open '$Open' failed (current app = '$($r.app)')"; exit 1 }
}

if ($Tap)   {
    $p = $Tap -split '[,x ]+' | Where-Object { $_ -ne "" }
    if ($p.Count -lt 2) { Write-Error "-Tap wants 'x,y'"; exit 1 }
    Api "/api/ui/tap?x=$($p[0])&y=$($p[1])" | Out-Null
}

Start-Sleep -Milliseconds $Wait

# Capture unless we only queried state. Default to a temp path when an action ran without -Out.
if (-not $Out -and ($Open -or $Tap -or $GoHome)) {
    $Out = Join-Path $env:TEMP ("nvshot_{0}.jpg" -f (Get-Date -Format "HHmmss"))
}
if ($Out) {
    Invoke-WebRequest -Uri "$base/api/screen" -OutFile $Out -TimeoutSec 15 | Out-Null
    $st = Api "/api/ui/state"
    $len = (Get-Item $Out).Length
    Write-Output ("SHOT {0} | app={1} | {2} bytes" -f $Out, $st.app, $len)
}
