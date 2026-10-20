# One-mode OTA bench run: flash BOTH boards with the same capture build,
# then dual-port monitor at 921600 with per-port logs. Usage:
#   powershell -File tools\capture\ota_bench.ps1 -Mode ccmp
# Modes: plain | ccmp | slim   (Mapping: slim = CEEPEW full, ccmp = CCMP PMK/LMK, plain = cleartext)
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('plain','ccmp','slim')]
    [string]$Mode
)
$ErrorActionPreference = 'Continue'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0.2\venv'
. C:\esp\v6.0.2\esp-idf\export.ps1

switch ($Mode) {
    'plain' { $dir = 'build_cap_plain' }
    'ccmp'  { $dir = 'build_cap_ccmp' }
    'slim'  { $dir = 'build_cap_slim' }
}
New-Item -ItemType Directory -Force -Path logs\ota_capture | Out-Null
$stamp = Get-Date -Format 'MMdd-HHmm'

Write-Host "=== FLASH [$Mode] ($dir) -> COM3 $(Get-Date -Format HH:mm:ss) ==="
& idf.py -B $dir -p COM3 flash 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath "logs\ota_capture\flash_COM3_$Mode.log" | Out-Null
Write-Host "=== FLASH [$Mode] -> COM5 $(Get-Date -Format HH:mm:ss) ==="
& idf.py -B $dir -p COM5 flash 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath "logs\ota_capture\flash_COM5_$Mode.log" | Out-Null

Write-Host "=== MONITOR [$Mode] dual-port starts $(Get-Date -Format HH:mm:ss), 900 s ==="
& python tools\ceepew_monitor.py --port COM3 --port COM5 --baud 921600 --duration 900 --log-per-port --log "logs\ota_capture\monitor_$($stamp)_$Mode.txt" --no-color 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath "logs\ota_capture\monitor_out_$Mode.log" | Out-Null
Write-Host "=== MONITOR [$Mode] done $(Get-Date -Format HH:mm:ss) ==="
Get-ChildItem logs\ota_capture\*_$($stamp)_$Mode* | Format-Table Name,Length -AutoSize | Out-String -Width 120
