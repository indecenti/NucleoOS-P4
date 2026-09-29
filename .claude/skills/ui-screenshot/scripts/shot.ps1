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
#   shot.ps1 -Tap "512,300" -Type "basic p.bas" -Key enter -Out term.jpg
#       # tap a text field to focus it, type a whole line in one call (no per-letter taps),
#       # then press enter to submit — same as typing on the on-screen keyboard.
param(
    [string]$Ip   = $(if ($env:NV_BOARD_IP) { $env:NV_BOARD_IP } else { "192.168.1.50" }),
    [string]$Open,                 # native app id to open (settings/files/sysmon/...)
    [string]$Tap,                  # synthetic pointer tap, "x,y" (0..1023 , 0..599)
    [string]$Type,                 # literal text into the focused field (tap it first) — one call,
                                    # no per-letter on-screen-keyboard taps
    [string]$Key,                  # a special key on the focused field: enter/esc/backspace/
                                    # delete/tab/left/right/up/down
    [switch]$GoHome,               # return to the launcher first
    [switch]$State,                # print current app id as JSON and exit (no capture)
    [string]$Out,                  # save the screenshot here (JPEG). Defaults to a temp file
    [int]$Wait    = 800            # ms to wait before capture (let the UI settle / animate)
)
$ErrorActionPreference = "Stop"
$base = "http://$Ip"
# Session token for the web API (python tools\pair.py writes it; NUCLEO_TOKEN overrides).
$tok = if ($env:NUCLEO_TOKEN) { $env:NUCLEO_TOKEN } else { $tf = Join-Path $env:USERPROFILE '.nucleo\token'; if (Test-Path $tf) { (Get-Content $tf -Raw).Trim() } else { '' } }
$AuthH = if ($tok) { @{ Authorization = "Bearer $tok" } } else { @{} }

function Api($path) {
    try { return Invoke-RestMethod -Uri "$base$path" -Headers $AuthH -TimeoutSec 8 }
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

if ($Type)  {
    $r = Api "/api/ui/type?text=$([uri]::EscapeDataString($Type))"
    if (-not $r.ok) { Write-Error "type failed: no field focused (tap one first with -Tap)"; exit 1 }
}

if ($Key)   {
    $r = Api "/api/ui/key?code=$([uri]::EscapeDataString($Key.ToLower()))"
    if (-not $r.ok) { Write-Error "key '$Key' failed: no field focused, or unknown code"; exit 1 }
}

Start-Sleep -Milliseconds $Wait

# Capture unless we only queried state. Default to a temp path when an action ran without -Out.
if (-not $Out -and ($Open -or $Tap -or $Type -or $Key -or $GoHome)) {
    $Out = Join-Path $env:TEMP ("nvshot_{0}.jpg" -f (Get-Date -Format "HHmmss"))
}
if ($Out) {
    Invoke-WebRequest -Uri "$base/api/screen" -Headers $AuthH -OutFile $Out -TimeoutSec 15 | Out-Null
    $st = Api "/api/ui/state"
    $len = (Get-Item $Out).Length
    Write-Output ("SHOT {0} | app={1} | {2} bytes" -f $Out, $st.app, $len)
}
