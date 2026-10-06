# sync-sd.ps1 — copia additiva del mirror sd/ del progetto sulla microSD.
# Non cancella mai nulla dalla card (niente /MIR): i file scritti dal sistema
# (apps/, nucleos/settings.nvb, foto, note) restano intatti.
#
# Uso:  .\tools\sync-sd.ps1 -Drive E:
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z]:$')]
    [string]$Drive
)

$src = Join-Path $PSScriptRoot '..\sd'
$src = (Resolve-Path $src).Path

if (-not (Test-Path "$Drive\")) {
    Write-Error "Unita' $Drive non trovata."
    exit 1
}

# Mai sulla card: i file che il dispositivo scrive da se' (dati e segreti dell'utente, alcuni cifrati
# sul chip) e quelli con chiavi. Una copia in chiaro nel mirror sovrascriverebbe quella cifrata.
$userData = @('README.md', 'teacher.json', 'telegram.json', 'workspace.json', 'session.txt', 'context.json',
              'memory.jsonl', 'MEMORY.md', 'SOUL.md', 'USER.md', 'HEARTBEAT.md', 'profile.tsv', 'rules.json',
              'timers.json', 'permissions.json', 'chatlog.ndjson', 'telemetry.ndjson', 'phrases.user.tsv',
              'user.tsv', 'user.vec', 'units.txt', 'agent_last.txt', 'settings.nvb', 'tele.bin', 'perms.json')
Write-Host "Sync $src -> $Drive\ (copia additiva)"
robocopy $src "$Drive\" /E /XO /R:1 /W:1 /NFL /NDL /NP /XF $userData
if ($LASTEXITCODE -ge 8) {
    Write-Error "robocopy fallita (codice $LASTEXITCODE)"
    exit $LASTEXITCODE
}
Write-Host "Fatto. Espelli la card in sicurezza prima di rimuoverla."
exit 0
