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

Write-Host "Sync $src -> $Drive\ (copia additiva)"
robocopy $src "$Drive\" /E /XO /R:1 /W:1 /NFL /NDL /NP /XF 'README.md'
if ($LASTEXITCODE -ge 8) {
    Write-Error "robocopy fallita (codice $LASTEXITCODE)"
    exit $LASTEXITCODE
}
Write-Host "Fatto. Espelli la card in sicurezza prima di rimuoverla."
exit 0
