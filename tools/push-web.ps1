# push-web.ps1 — deploy dei file web modificati sulla board VIA WI-FI (niente card reader).
# Usa POST /api/web/put?path=<rel>: scrive su /sdcard/web/<rel> E aggiorna la cache PSRAM live,
# quindi il file è servito subito e persiste ai riavvii.
#
# Uso:  .\tools\push-web.ps1 -BoardIp 192.168.0.xxx            # push dei file di default
#       .\tools\push-web.ps1 -BoardIp auto                     # scansiona la /24 e trova la board
#       .\tools\push-web.ps1 -BoardIp auto -Files web\ai.js    # file specifici (relativi a sd\)
param(
    [string]$BoardIp = 'auto',
    [string[]]$Files = @('web\copilot.js', 'web\ai.js', 'web\apps\agent\runtime.js', 'web\apps\games\llm.js')
)
$ErrorActionPreference = 'Stop'
$sd = (Resolve-Path (Join-Path $PSScriptRoot '..\sd')).Path

if ($BoardIp -eq 'auto') {
    Write-Host 'Cerco la board sulla /24…'
    $found = $null
    try { $r = Invoke-RestMethod -Uri 'http://nucleov2.local/api/info' -TimeoutSec 3; $found = 'nucleov2.local' } catch {}
    if (-not $found) {
        $tasks = @{}
        foreach ($i in 2..254) { $ip = "192.168.0.$i"; $c = New-Object System.Net.Sockets.TcpClient; $tasks[$ip] = @{ c = $c; t = $c.ConnectAsync($ip, 80) } }
        Start-Sleep -Milliseconds 1200
        $open = @(); foreach ($k in $tasks.Keys) { if ($tasks[$k].t.IsCompleted -and $tasks[$k].c.Connected) { $open += $k }; $tasks[$k].c.Close() }
        foreach ($ip in $open) {
            try { $r = Invoke-RestMethod -Uri "http://$ip/api/info" -TimeoutSec 2; if ($null -ne $r.version) { $found = $ip; break } } catch {}
        }
    }
    if (-not $found) { Write-Error 'Board non trovata (accesa e sul Wi-Fi?)'; exit 1 }
    $BoardIp = $found
    Write-Host "Board: $BoardIp"
}

$ok = 0
foreach ($f in $Files) {
    $src = Join-Path $sd $f
    if (-not (Test-Path $src)) { Write-Warning "manca: $src"; continue }
    # path lato board = relativo a /sdcard/web → togli il prefisso web\
    $rel = ($f -replace '^web[\\/]', '') -replace '\\', '/'
    $url = "http://$BoardIp/api/web/put?path=$([uri]::EscapeDataString($rel))"
    try {
        Invoke-RestMethod -Uri $url -Method Post -InFile $src -ContentType 'application/octet-stream' -TimeoutSec 30 | Out-Null
        $sz = (Get-Item $src).Length
        Write-Host ("OK  {0}  ({1} B)" -f $rel, $sz)
        $ok++
    } catch { Write-Warning ("FAIL {0}: {1}" -f $rel, $_.Exception.Message) }
}
Write-Host "PUSHED $ok/$($Files.Count) su $BoardIp (serviti subito, cache live)"
