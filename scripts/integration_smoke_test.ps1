# Integration smoke test helper for CEE-PEW
# Usage: .\integration_smoke_test.ps1 -Port COM3
param(
    [string]$Port = "COM3"
)

Write-Output "Checking for idf.py in PATH..."
$idf = Get-Command idf.py -ErrorAction SilentlyContinue
if (-not $idf) {
    Write-Output "idf.py not found in PATH. Please install ESP-IDF and run this script from an environment with idf.py available."
    Write-Output "Alternatively, apply the patch file in session-state/files/ble-ui-fixes.patch and build on your dev machine."
    exit 2
}

Write-Output "Running idf.py build..."
$build = & idf.py build
if ($LASTEXITCODE -ne 0) {
    Write-Output "Build failed. Inspect output above."
    exit $LASTEXITCODE
}

Write-Output "Flashing to device on port $Port (requires device connected)."
Write-Output "To flash manually: idf.py -p $Port flash monitor"

# Attempt to flash if user confirms
$yn = Read-Host "Proceed to flash and start monitor? (y/N)"
if ($yn -eq 'y' -or $yn -eq 'Y') {
    & idf.py -p $Port flash monitor
}
else {
    Write-Output "Skipping flash. Build artifacts are in build/ directory."
}
