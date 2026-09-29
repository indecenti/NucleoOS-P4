# decode-coredump.ps1 - pull the core dump from the device and decode it against an ELF
# (task list, registers, backtrace with symbols).
#
# Usage:
#   .\tools\decode-coredump.ps1 -Url 192.168.1.50     # over Wi-Fi (/api/crash/dump): no cable, no reset
#   .\tools\decode-coredump.ps1                        # over serial, COM5 (esptool RESETS the board)
#   .\tools\decode-coredump.ps1 -Port COM7 -Elf path\to\nucleos-anima.elf
#
# The ELF must be the exact image that crashed: espcoredump refuses a SHA mismatch. GET /api/crash
# shows the dump's "elf_sha" and whether it is the running build ("this_build"); for an older image,
# rebuild its release commit in a worktree and pass -Elf. The raw dump is kept (-Out) either way.
[CmdletBinding()]
param(
    [string]$Port = 'COM5',
    [string]$Url = '',
    [string]$Elf = '',
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Elf) { $Elf = Join-Path $root 'build\nucleos-anima.elf' }
if (-not (Test-Path $Elf)) { throw "ELF not found: $Elf (build first)" }

$env:IDF_TOOLS_PATH = 'D:\esp\tools'
. 'D:\esp\esp-idf-v5.5.2\export.ps1' | Out-Null

if ($Url) {
    $base = if ($Url -match '^https?://') { $Url.TrimEnd('/') } else { "http://$Url" }
    $info = Invoke-RestMethod -Uri "$base/api/crash" -TimeoutSec 15
    if (-not $info.present) { Write-Host 'No core dump stored on the device.'; return }
    Write-Host ("crash: {0} in task '{1}' @ {2} (ra {3}, sp {4}), elf {5}, this_build={6}" -f `
        $info.reason, $info.task, $info.pc, $info.ra, $info.sp, $info.elf_sha, $info.this_build)
    if (-not $Out) { $Out = Join-Path $env:TEMP ("coredump-{0}.bin" -f $info.elf_sha) }
    Invoke-WebRequest -Uri "$base/api/crash/dump" -OutFile $Out -TimeoutSec 60
    Write-Host "raw dump saved: $Out ($((Get-Item $Out).Length) bytes)"
    python -m esp_coredump --chip esp32p4 info_corefile --core $Out --core-format raw $Elf
} else {
    python -m esp_coredump --chip esp32p4 --port $Port info_corefile $Elf
}
