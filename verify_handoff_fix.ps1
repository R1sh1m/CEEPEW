# verify_handoff_fix.ps1
# Build & verify the optimistic-handoff + event-driven-tick fix.
# Run from a PowerShell with the IDF env sourced.
#
# Usage:  .\verify_handoff_fix.ps1            # default: build only
#         .\verify_handoff_fix.ps1 -Flash     # build + flash both ports
#         .\verify_handoff_fix.ps1 -Monitor   # build + monitor port 5

param(
    [switch]$Flash,
    [switch]$Monitor
)

$ErrorActionPreference = "Stop"
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1' | Out-Null

Set-Location -Path $PSScriptRoot

Write-Host "=== idf.py build ===" -ForegroundColor Cyan
idf.py build 2>&1 | Select-Object -Last 40
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed."
    exit 1
}

if ($Flash) {
    Write-Host "=== idf.py flash COM5 ===" -ForegroundColor Cyan
    idf.py flash -p COM5
    if ($LASTEXITCODE -ne 0) { Write-Error "Flash failed."; exit 1 }
    Write-Host "=== idf.py flash COM6 ===" -ForegroundColor Cyan
    idf.py flash -p COM6
    if ($LASTEXITCODE -ne 0) { Write-Error "Flash failed."; exit 1 }
}

if ($Monitor) {
    Write-Host "=== idf.py monitor COM5 ===" -ForegroundColor Cyan
    idf.py monitor -p COM5
}

Write-Host ""
Write-Host "=== Manual pairing tests to perform ===" -ForegroundColor Yellow
Write-Host "1. MISMATCH test: type DIFFERENT codes on the two units."
Write-Host "   EXPECT: BOTH units transition to PAIRING_FAILED, neither enters CHAT."
Write-Host "   (This is the regression for Issue A. Pre-fix: one device entered CHAT.)"
Write-Host ""
Write-Host "2. MATCH test: type the SAME code on both units."
Write-Host "   EXPECT: BOTH units enter CHAT within ~500ms of last commit."
Write-Host ""
Write-Host "3. Watch monitor for: 'queue_timeouts' (should be rare) and the new"
Write-Host "   'Initiator: GATTC write ACK received — awaiting peer verification result'"
Write-Host "   log line (replaces the old 'handoff_ready set on write ACK' line)."
