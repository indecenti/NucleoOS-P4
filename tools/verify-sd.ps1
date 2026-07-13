<#
verify-sd.ps1 — verifica (e opzionalmente ripara) che la microSD della board contenga
                TUTTI i file del mirror di progetto `sd/`, byte per byte.

UNO strumento, DUE trasporti (identico report e logica in entrambi):
  * Card reader:  -Drive E:            la SD e' inserita nel PC
  * Wi-Fi API:    -Api 192.168.0.128   la board e' accesa e sul Wi-Fi (nv_web)
                  -Api auto            scansiona la /24 e trova la board

Cosa fa:
  1. Costruisce un manifest del repo `sd/` (path relativo + dimensione [+ sha256 con -Deep]).
  2. Confronta col target elencando/leggendo i file:
       - Card reader: legge il filesystem della SD.
       - API: enumera l'albero con /api/fs/list (dimensioni) in modo SERIALE
              (l'httpd del P4 ha ~16 socket: le richieste parallele lo saturano
               e ritornano codice 000 -> falsi "MISSING").
  3. Classifica ogni file: OK / MISSING / DIFF (dimensione) / DIFFHASH (-Deep) / TOOBIG.
  4. Con -Fix copia/pusha SOLO i mancanti/diversi. Additivo: non cancella mai nulla dal target.

Vincoli lato device (rispettati automaticamente):
  * ramo `web/`  -> POST /api/web/put   (cap 8 MB, aggiorna anche la cache PSRAM live)
  * altri rami   -> POST /api/fs/write  (cap 64 MB; oltre = TOOBIG, serve card reader)

Esempi:
  .\tools\verify-sd.ps1 -Api auto                     # verifica via Wi-Fi
  .\tools\verify-sd.ps1 -Api 192.168.0.128 -Fix       # verifica e ripara via Wi-Fi
  .\tools\verify-sd.ps1 -Drive E: -Fix                # verifica e ripara via card reader
  .\tools\verify-sd.ps1 -Drive E: -Deep               # confronto profondo (sha256) su tutto
  .\tools\verify-sd.ps1 -Api auto -Only web,apps      # solo alcuni rami
#>
[CmdletBinding(DefaultParameterSetName = 'Api')]
param(
    [Parameter(ParameterSetName = 'Drive', Mandatory = $true)]
    [ValidatePattern('^[A-Za-z]:$')]
    [string]$Drive,

    [Parameter(ParameterSetName = 'Api', Mandatory = $true)]
    [string]$Api,

    [switch]$Fix,
    [switch]$Force,                # con -Fix, ripara anche i DIFF (default: solo i MISSING)
    [switch]$Deep,
    [string[]]$Only,
    [string[]]$Exclude,            # glob (wildcard su Rel) da NON toccare, es. 'apps/tanks/*','data/anima/teacher.json'
    [int]$ApiTimeoutSec = 60
)
$ErrorActionPreference = 'Stop'
$sd = (Resolve-Path (Join-Path $PSScriptRoot '..\sd')).Path

# ------------------------------------------------------------------ manifest repo
Write-Host "Manifest di $sd ..." -ForegroundColor Cyan
$manifest = @()
foreach ($fi in Get-ChildItem -LiteralPath $sd -Recurse -File) {
    if ($fi.Name -eq 'README.md') { continue }          # sync-sd esclude il README
    $rel = $fi.FullName.Substring($sd.Length + 1) -replace '\\', '/'
    $branch = ($rel -split '/', 2)[0]
    if ($Only -and ($Only -notcontains $branch)) { continue }
    if ($Exclude) { $skip = $false; foreach ($g in $Exclude) { if ($rel -like $g) { $skip = $true; break } }; if ($skip) { continue } }
    $h = $null
    if ($Deep) { $h = (Get-FileHash -LiteralPath $fi.FullName -Algorithm SHA256).Hash }
    $manifest += [pscustomobject]@{ Rel = $rel; Full = $fi.FullName; Size = [int64]$fi.Length; Hash = $h; Branch = $branch }
}
$branches = ($manifest | Select-Object -ExpandProperty Branch -Unique) | Sort-Object
Write-Host ("  {0} file, rami: {1}" -f $manifest.Count, ($branches -join ', '))

# ------------------------------------------------------------------ helper API
function Resolve-Board([string]$hint) {
    if ($hint -ne 'auto') { return $hint }
    Write-Host 'Cerco la board sulla /24 ...'
    try { $null = Invoke-RestMethod -Uri 'http://nucleov2.local/api/info' -TimeoutSec 3; return 'nucleov2.local' } catch {}
    $tasks = @{}
    foreach ($i in 2..254) { $ip = "192.168.0.$i"; $c = New-Object System.Net.Sockets.TcpClient; $tasks[$ip] = @{ c = $c; t = $c.ConnectAsync($ip, 80) } }
    Start-Sleep -Milliseconds 1200
    $open = @(); foreach ($k in $tasks.Keys) { if ($tasks[$k].t.IsCompleted -and $tasks[$k].c.Connected) { $open += $k }; $tasks[$k].c.Close() }
    foreach ($ip in $open) { try { $r = Invoke-RestMethod -Uri "http://$ip/api/info" -TimeoutSec 2; if ($null -ne $r.version) { return $ip } } catch {} }
    throw 'Board non trovata (accesa e sul Wi-Fi?)'
}

function Api-Get([string]$url) {
    for ($a = 0; $a -lt 4; $a++) {
        try { return Invoke-RestMethod -Uri $url -TimeoutSec 20 }
        catch { Start-Sleep -Milliseconds (250 * ($a + 1)) }
    }
    throw "GET fallita: $url"
}

# Enumerazione SERIALE dell'albero board sotto /<branch>. Ritorna hashtable rel->size.
function Get-BoardTree([string]$ip, [string[]]$roots) {
    $out = @{}
    $stack = New-Object System.Collections.Stack
    foreach ($r in $roots) { $stack.Push("/$r") }
    while ($stack.Count -gt 0) {
        $p = $stack.Pop()
        $j = Api-Get ("http://{0}/api/fs/list?path={1}" -f $ip, [uri]::EscapeDataString($p))
        foreach ($e in $j.entries) {
            $child = "$p/$($e.name)"
            if ($e.isDir) { $stack.Push($child) }
            else { $out[$child.TrimStart('/')] = [int64]$e.size }
        }
    }
    return $out
}

# sha256 di un file remoto (scarica in temp; fs/read serve sempre bytes grezzi, niente gz).
function Board-Hash([string]$ip, [string]$rel) {
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        Invoke-WebRequest -Uri ("http://{0}/api/fs/read?path=/{1}" -f $ip, [uri]::EscapeDataString($rel)) `
            -Headers @{ 'Accept-Encoding' = 'identity' } -OutFile $tmp -TimeoutSec $ApiTimeoutSec | Out-Null
        return (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
    } finally { Remove-Item $tmp -ErrorAction SilentlyContinue }
}

# push di un file verso la board (sceglie l'endpoint giusto e rispetta i cap). Ritorna $true/$false.
function Push-File([string]$ip, [pscustomobject]$m) {
    if ($m.Branch -eq 'web') {
        if ($m.Size -gt 8MB) { return $false }             # cap web put
        $rel = $m.Rel -replace '^web/', ''
        Invoke-RestMethod -Uri ("http://{0}/api/web/put?path={1}" -f $ip, [uri]::EscapeDataString($rel)) `
            -Method Post -InFile $m.Full -ContentType 'application/octet-stream' -TimeoutSec $ApiTimeoutSec | Out-Null
    } else {
        if ($m.Size -gt 64MB) { return $false }            # cap fs write
        Invoke-RestMethod -Uri ("http://{0}/api/fs/write?path=/{1}" -f $ip, [uri]::EscapeDataString($m.Rel)) `
            -Method Post -InFile $m.Full -ContentType 'application/octet-stream' -TimeoutSec $ApiTimeoutSec | Out-Null
    }
    return $true
}

function Push-Cap([pscustomobject]$m) { if ($m.Branch -eq 'web') { 8MB } else { 64MB } }

# ------------------------------------------------------------------ verifica
$results = @()   # {Rel, Status, Local, Remote, Fixed}
$ip = $null

if ($PSCmdlet.ParameterSetName -eq 'Api') {
    $ip = Resolve-Board $Api
    Write-Host "Board: $ip (verifica via Wi-Fi, seriale)" -ForegroundColor Cyan
    $board = Get-BoardTree $ip $branches
    Write-Host ("  albero board: {0} file sotto i rami verificati" -f $board.Count)
    foreach ($m in $manifest) {
        if (-not $board.ContainsKey($m.Rel)) { $status = 'MISSING'; $rem = $null }
        elseif ($board[$m.Rel] -ne $m.Size) { $status = 'DIFF'; $rem = $board[$m.Rel] }
        else {
            $rem = $m.Size; $status = 'OK'
            if ($Deep) { if ((Board-Hash $ip $m.Rel) -ne $m.Hash) { $status = 'DIFFHASH' } }
        }
        if ($status -ne 'OK' -and $m.Size -gt (Push-Cap $m)) { $status = 'TOOBIG' }
        $results += [pscustomobject]@{ Rel = $m.Rel; Status = $status; Local = $m.Size; Remote = $rem; M = $m }
    }
} else {
    if (-not (Test-Path "$Drive\")) { throw "Unita' $Drive non trovata." }
    Write-Host "Target: $Drive\ (verifica via card reader)" -ForegroundColor Cyan
    foreach ($m in $manifest) {
        $dest = Join-Path "$Drive\" ($m.Rel -replace '/', '\')
        if (-not (Test-Path -LiteralPath $dest)) { $status = 'MISSING'; $rem = $null }
        else {
            $di = Get-Item -LiteralPath $dest; $rem = [int64]$di.Length
            if ($rem -ne $m.Size) { $status = 'DIFF' }
            elseif ($Deep -and (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash -ne $m.Hash) { $status = 'DIFFHASH' }
            else { $status = 'OK' }
        }
        $results += [pscustomobject]@{ Rel = $m.Rel; Status = $status; Local = $m.Size; Remote = $rem; M = $m }
    }
}

# ------------------------------------------------------------------ report
$anom = $results | Where-Object Status -ne 'OK'
$byBranch = $results | Group-Object { ($_.Rel -split '/', 2)[0] }
Write-Host ''
Write-Host '== Riepilogo per ramo ==' -ForegroundColor Cyan
foreach ($g in ($byBranch | Sort-Object Name)) {
    $ok = ($g.Group | Where-Object Status -eq 'OK').Count
    $bad = $g.Count - $ok
    $col = if ($bad -eq 0) { 'Green' } else { 'Yellow' }
    Write-Host ("  {0,-10} {1,4}/{2,-4} ok" -f $g.Name, $ok, $g.Count) -ForegroundColor $col
}
if ($anom.Count -gt 0) {
    Write-Host ''
    Write-Host "== Anomalie ($($anom.Count)) ==" -ForegroundColor Yellow
    $anom | Sort-Object Status, Rel | Select-Object -First 200 |
        Format-Table @{L='Stato';E={$_.Status}}, @{L='Locale';E={$_.Local}}, @{L='Board';E={$_.Remote}}, Rel -AutoSize | Out-Host
} else {
    Write-Host ''
    Write-Host 'TUTTO SINCRONIZZATO: nessun file mancante o diverso.' -ForegroundColor Green
}

# ------------------------------------------------------------------ fix
if ($Fix -and $anom.Count -gt 0) {
    $toobig  = $anom | Where-Object Status -eq 'TOOBIG'
    # Di default si riparano SOLO i MISSING (inequivocabili). I DIFF possono voler dire
    # "il board ha una versione piu' nuova o una config propria" (es. tanks ricompilato,
    # teacher.json con la chiave): richiedono -Force per essere sovrascritti.
    $diffs   = $anom | Where-Object { $_.Status -in 'DIFF','DIFFHASH' }
    if ($Force) { $fixable = $anom | Where-Object Status -ne 'TOOBIG' }
    else        { $fixable = $anom | Where-Object Status -eq 'MISSING' }
    Write-Host ''
    Write-Host "== Riparazione di $($fixable.Count) file ==" -ForegroundColor Cyan
    if (-not $Force -and $diffs.Count -gt 0) {
        Write-Warning "$($diffs.Count) DIFF NON toccati (il board potrebbe avere una versione piu' recente). Usa -Force per sovrascriverli, o -Exclude per protezione mirata."
    }
    $done = 0; $fail = 0
    foreach ($r in $fixable) {
        try {
            if ($ip) {
                if (Push-File $ip $r.M) { $done++; Write-Host ("  OK   {0}" -f $r.Rel) }
                else { $fail++; Write-Warning ("  cap superato: {0}" -f $r.Rel) }
            } else {
                $dest = Join-Path "$Drive\" ($r.Rel -replace '/', '\')
                $pdir = Split-Path $dest -Parent
                if (-not (Test-Path -LiteralPath $pdir)) { New-Item -ItemType Directory -Path $pdir -Force | Out-Null }
                Copy-Item -LiteralPath $r.M.Full -Destination $dest -Force
                $done++; Write-Host ("  OK   {0}" -f $r.Rel)
            }
        } catch { $fail++; Write-Warning ("  FAIL {0}: {1}" -f $r.Rel, $_.Exception.Message) }
    }
    Write-Host ("Riparati {0}, falliti {1}" -f $done, $fail) -ForegroundColor Cyan
    if ($toobig.Count -gt 0) {
        Write-Host ''
        Write-Warning "$($toobig.Count) file oltre il cap API: pushabili SOLO via card reader (-Drive):"
        $toobig | ForEach-Object { Write-Host ("    {0}  ({1:N0} B)" -f $_.Rel, $_.Local) }
    }
}

# exit code: 0 = pulito (dopo eventuale fix), 1 = anomalie residue
if ($Fix) {
    # residui = cio' che NON e' stato riparato: TOOBIG sempre, DIFF se non -Force
    $residual = @($anom | Where-Object Status -eq 'TOOBIG')
    if (-not $Force) { $residual += @($anom | Where-Object { $_.Status -in 'DIFF','DIFFHASH' }) }
    exit ([int]($residual.Count -gt 0))
}
exit ([int]($anom.Count -gt 0))
