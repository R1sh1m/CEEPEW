# Finish the rebuild: idempotent ccmp, full slim, then size+sha256 for all.
$ErrorActionPreference = 'Continue'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0.2\venv'
. C:\esp\v6.0.2\esp-idf\export.ps1

$modes = @(
    @{ name='ccmp';  dir='build_cap_ccmp';  defaults='sdkconfig.defaults;C:/esp/cap_ccmp.defaults';
       need=@('CONFIG_CEEPEW_CAPTURE 1','CONFIG_CEEPEW_CAPTURE_MODE_CCMP 1','CONFIG_CEEPEW_BENCH_58B 1') },
    @{ name='slim';  dir='build_cap_slim';  defaults='sdkconfig.defaults;build_cap_slim/cap.defaults';
       need=@('CONFIG_CEEPEW_CAPTURE 1','CONFIG_CEEPEW_BENCH_58B 1') },
    @{ name='plain'; dir='build_cap_plain'; defaults='sdkconfig.defaults;C:/esp/cap_plain.defaults';
       need=@('CONFIG_CEEPEW_CAPTURE 1','CONFIG_CEEPEW_CAPTURE_MODE_PLAIN 1','CONFIG_CEEPEW_BENCH_58B 1') }
)

function Gate([string]$dir, [string[]]$need) {
    $hdr = Join-Path $dir 'config\sdkconfig.h'
    if (-not (Test-Path $hdr)) { return "NO-HEADER $dir" }
    $header = Get-Content $hdr -Raw
    $missing = @()
    foreach ($sym in $need) { if (-not $header.Contains("#define $sym")) { $missing += $sym } }
    if ($missing.Count -gt 0) { return "GATE-FAIL $dir missing $($missing -join ', ')" }
    return "GATE-OK $dir"
}

$results = @()
foreach ($m in $modes) {
    $d = $m.dir
    $log = "tools\capture\rebuild2_$($m.name).log"
    Write-Host "=== $($m.name) start $(Get-Date -Format HH:mm:ss) ==="
    & idf.py -B $d -D "SDKCONFIG=$d/sdkconfig" -D "SDKCONFIG_DEFAULTS=$($m.defaults)" reconfigure 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log -Append | Out-Null
    $g = Gate $d $m.need
    Write-Host $g
    if ($g -notlike 'GATE-OK*') { $results += $g; break }

    & idf.py -B $d build 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log -Append | Out-Null
    $bin = Join-Path $d 'ceepew.bin'
    if (-not (Test-Path $bin)) { Write-Host "!!! BUILD FAIL $($m.name)"; $results += "BUILD-FAIL $($m.name)"; break }
    $mt = Get-Date (Get-Item $bin).LastWriteTime -Format 'HH:mm:ss'
    Write-Host "=== $($m.name) bin present, mtime $mt ==="
}

Write-Host '=== SIZE + HASH PASS (idf.py size per mode; sha256 via python) ==='
foreach ($m in $modes) {
    $d = $m.dir
    & idf.py -B $d size 2>&1 | ForEach-Object { "$_" } | Select-String -Pattern 'Total sizes|DRAM|diram|Used static' -Context 0,6 | Out-String -Width 200 | Set-Content "tools\capture\size_$($m.name).txt"
    $bytes = Get-Content "tools\capture\size_$($m.name).txt" -Raw
    Write-Host "--- $($m.name) size ---`n$bytes"
}
foreach ($m in $modes) {
    $bin = Join-Path $m.dir 'ceepew.bin'
    $b = [System.IO.File]::ReadAllBytes($bin)
    $sha = [System.Security.Cryptography.SHA256]::Create().ComputeHash($b)
    $hex = ($sha | ForEach-Object { $_.ToString('x2') }) -join ''
    Write-Host "$($m.name) sha256=$hex"
}

# Map proofs
foreach ($m in $modes) {
    $map = Join-Path $m.dir 'ceepew.map'
    $t = Get-Content $map -Raw
    Write-Host "$($m.name) map_logger=$($t.Contains('transport_capture_logger_task')) map_bench=$($t.Contains('session_send_bench_forge_next'))"
}
Write-Host '=== DONE ==='
