# decode-coredump.ps1 - pull the core dump from the device flash over serial and decode it
# against the current ELF (task list, registers, backtrace with symbols).
#
# Usage:
#   .\tools\decode-coredump.ps1              # COM5, build\nucleos-anima.elf
#   .\tools\decode-coredump.ps1 -Port COM7
#
# Requires the device attached via the USB-Serial/JTAG port. The on-device Diagnostics app
# shows the quick summary (task + PC); this script is the full PC-side decode.
[CmdletBinding()]
param(
    [string]$Port = 'COM5',
    [string]$Elf = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Elf) { $Elf = Join-Path $root 'build\nucleos-anima.elf' }
if (-not (Test-Path $Elf)) { throw "ELF not found: $Elf (build first)" }

$env:IDF_TOOLS_PATH = 'D:\esp\tools'
. 'D:\esp\esp-idf-v5.5.2\export.ps1' | Out-Null

python "$env:IDF_PATH\components\espcoredump\espcoredump.py" `
    --chip esp32p4 --port $Port info_corefile $Elf
