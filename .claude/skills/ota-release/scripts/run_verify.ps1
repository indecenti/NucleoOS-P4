# Activate ESP-IDF (its python env ships pyserial) and run the compact OTA verifier.
# export.ps1 + native python emit stderr that PS 5.1 wraps as errors; keep going past it.
$ErrorActionPreference = 'Continue'
$env:IDF_TOOLS_PATH = 'D:\esp\tools'
. 'D:\esp\esp-idf-v5.5.2\export.ps1' *> $null
python (Join-Path $PSScriptRoot 'verify.py')
