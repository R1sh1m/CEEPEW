$ErrorActionPreference = 'Continue'
Set-Location 'C:\Users\Rishi Misra\Desktop\Code\Projects\CEEPEW'
$log = 'data\raw\_tooling'
New-Item -ItemType Directory -Force -Path $log | Out-Null

. .\tools\setup_esp_idf.ps1

"=== BUILD build_cap_ccmp START $(Get-Date -Format o) ===" | Out-File "$log\build_ccmp_fix.log" -Encoding utf8
idf.py -B build_cap_ccmp build 2>&1 | Out-File "$log\build_ccmp_fix.log" -Append -Encoding utf8
$rc = $LASTEXITCODE
"=== BUILD build_cap_ccmp END $(Get-Date -Format o) rc=$rc ===" | Out-File "$log\build_ccmp_fix.log" -Append -Encoding utf8
if (Test-Path 'build_cap_ccmp\ceepew.bin') {
    "build_cap_ccmp OK $((Get-Item 'build_cap_ccmp\ceepew.bin').Length)" | Out-File "$log\ccmp_done.txt" -Encoding utf8
} else {
    "build_cap_ccmp FAIL rc=$rc" | Out-File "$log\ccmp_done.txt" -Encoding utf8
}
