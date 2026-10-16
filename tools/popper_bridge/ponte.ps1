# Il ponte Popper <-> scheda DMD (nucleo_bridge.py): avvio con Windows, stato, avvio/arresto a mano.
#   powershell -NoProfile -ExecutionPolicy Bypass -File ponte.ps1 installa|rimuovi|avvia|ferma|riavvia|stato|controllo
# installa: parte a ogni accesso di Windows (attivita' pianificata, senza finestre, priorita' bassa) e subito.
# Il registro e' in %LOCALAPPDATA%\nucleo_bridge\ponte.log.
param([ValidateSet('installa', 'rimuovi', 'avvia', 'ferma', 'riavvia', 'stato', 'controllo')][string]$cmd = 'stato')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$task = 'NucleoOS DMD ponte'
$data = Join-Path $env:LOCALAPPDATA 'nucleo_bridge'
New-Item -ItemType Directory -Force $data | Out-Null
$log = Join-Path $data 'ponte.log'
$py = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
if (-not (Test-Path $py)) { $py = (Get-Command python.exe -ErrorAction SilentlyContinue).Source }
$pyw = Join-Path (Split-Path $py) 'pythonw.exe'
if (-not (Test-Path $pyw)) { $pyw = $py }
$bridgeArgs = "-u `"$here\nucleo_bridge.py`" --log `"$log`""

# The running bridge, whoever started it (the logon task, this script, by hand); --check runs excluded.
function Bridge {
    Get-CimInstance Win32_Process -Filter "Name LIKE 'python%'" |
        Where-Object { $_.CommandLine -like '*nucleo_bridge.py*' -and $_.CommandLine -notlike '*--check*' }
}

function Board {
    Get-CimInstance Win32_PnPEntity | Where-Object { $_.DeviceID -like 'USB\VID_303A&PID_1001&MI_00*' } |
        ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } } | Select-Object -First 1
}

function Start-Bridge {
    if (Bridge) { "Il ponte e' gia' acceso (pid $((Bridge).ProcessId -join ', '))."; return }
    if (-not $py) { throw 'Python non trovato' }
    $port = Board
    if (-not $port) { "Attenzione: la scheda (USB 303A:1001) non risulta collegata; il ponte la aspetta." }
    if ((Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue)) {
        Start-ScheduledTask -TaskName $task    # same process as at logon: in the user's session, no window
    } else {
        # Created by WMI, not as our child: it outlives the shell that started it (an SSH session or a
        # Claude command, whose child processes Windows ends with them).
        $startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
        $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
            CommandLine = "`"$pyw`" $bridgeArgs"; CurrentDirectory = $here; ProcessStartupInformation = $startup }
        if ($r.ReturnValue -ne 0) { throw "avvio non riuscito (WMI $($r.ReturnValue))" }
    }
    Start-Sleep 3
    $b = Bridge
    if (-not $b) { "Il ponte si e' chiuso subito:"; if (Test-Path $log) { Get-Content $log -Tail 20 }; exit 1 }
    "Ponte acceso (pid $($b.ProcessId -join ', '))$(if ($port) { ", scheda su $port" })."
    if (Test-Path $log) { Get-Content $log -Tail 4 }
}

function Stop-Bridge {
    $b = Bridge
    if (-not $b) { "Il ponte e' gia' spento."; return }
    $b | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    "Ponte spento; la porta della scheda e' libera."
}

switch ($cmd) {
    'installa' {
        $action = New-ScheduledTaskAction -Execute $pyw -Argument $bridgeArgs -WorkingDirectory $here
        $me = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name   # PC\user (over SSH USERDOMAIN is WORKGROUP)
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $me
        # priority 7 = below normal; restarted if it ever dies; no time limit; one instance
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -Priority 7 `
            -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
        $principal = New-ScheduledTaskPrincipal -UserId $me -LogonType Interactive -RunLevel Limited
        Register-ScheduledTask -TaskName $task -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force | Out-Null
        "Installato: il ponte parte a ogni accesso di Windows ('$task')."
        Stop-Bridge | Out-Null
        Start-Bridge
    }
    'rimuovi' {
        Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
        Stop-Bridge
        "Rimosso l'avvio con Windows."
    }
    'avvia' { Start-Bridge }
    'ferma' { Stop-Bridge }
    'riavvia' { Stop-Bridge; Start-Sleep 1; Start-Bridge }
    'stato' {
        $b = Bridge
        if ($b) {
            $p = Get-Process -Id ($b | Select-Object -First 1).ProcessId
            "Ponte ACCESO: pid $($p.Id), avviato $($p.StartTime), CPU $([math]::Round($p.CPU, 1)) s, memoria $([math]::Round($p.WorkingSet64 / 1MB)) MB, priorita' $($p.PriorityClass)"
        } else { 'Ponte SPENTO' }
        $t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
        "Avvio con Windows: $(if ($t) { 'si' } else { 'no (ponte.ps1 installa)' })"
        $port = Board
        "Scheda: $(if ($port) { $port } else { 'non collegata' })"
        if (Test-Path $log) { '--- ultime righe del registro'; Get-Content $log -Tail 12 }
    }
    'controllo' {
        & $py "$here\nucleo_bridge.py" --check --report (Join-Path $data 'controllo.txt')
    }
}
