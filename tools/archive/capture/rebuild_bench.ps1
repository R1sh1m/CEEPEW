# Sequential rebuild of the three CEE-PEW capture bench firmwares with
# BENCH_58B + headless-via-capture enabled. Single owner; never run two
# instances concurrently. Hard gate: verified Kconfig symbols before build.
$ErrorActionPreference = 'Continue'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0.2\venv'
. C:\esp\v6.0.2\esp-idf\export.ps1

$modes = @(
    @{ name='plain'; dir='build_cap_plain'; defaults='sdkconfig.defaults;C:/esp/cap_plain.defaults';
       need=@('CONFIG_CEEPEW_CAPTURE 1','CONFIG_CEEPEW_CAPTURE_MODE_PLAIN 1','CONFIG_CEEPEW_BENCH_58B 1') },
    @{ name='ccmp';  dir='build_cap_ccmp';  defaults='sdkconfig.defaults;C:/esp/cap_ccmp.defaults';
       need=@('CONFIG_CEEPEW_CAPTURE 1','CONFIG_CEEPEW_CAPTURE_MODE_CCMP 1','CONFIG_CEEPEW_BENCH_58B 1') },
    @{ name='slim';  dir='build_cap_slim';  defaults='sdkconfig.defaults;build_cap_slim/cap.defaults';
       need=@('CONFIG_CEEPEW_CAPTURE 1','CONFIG_CEEPEW_BENCH_58B 1') }
)

$results = @()
foreach ($m in $modes) {
    $d = $m.dir
    $log = "tools\capture\rebuild_$($m.name).log"
    Write-Host "=== REBUILD $($m.name) ($d) start $(Get-Date -Format HH:mm:ss) ==="
    & idf.py -B $d -D "SDKCONFIG=$d/sdkconfig" -D "SDKCONFIG_DEFAULTS=$($m.defaults)" reconfigure 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log -Append | Out-Null

    # Hard gate on generated header BEFORE building (plain substring match)
    $hdr = Join-Path $d 'config\sdkconfig.h'
    $header = ''
    if (Test-Path $hdr) { $header = Get-Content $hdr -Raw }
    $missing = @()
    foreach ($sym in $m.need) {
        if (-not $header.Contains("#define $sym")) { $missing += $sym }
    }
    if ($missing.Count -gt 0) {
        Write-Host "!!! GATE FAIL $($m.name): missing $($missing -join ', ') - ABORT"
        $results += "GATE-FAIL $($m.name): missing $($missing -join ', ')"
        continue
    }
    Write-Host "GATE OK $($m.name): $($m.need -join ' | ')"

    & idf.py -B $d build 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log -Append | Out-Null
    $bin = Join-Path $d 'ceepew.bin'
    if (Test-Path $bin) {
        $size = (Get-Item $bin).Length
        $h = (Get-FileHash $bin -Algorithm SHA256).Hash.ToLower()
        $map = Join-Path $d 'ceepew.map'
        $mapTxt = ''
        if (Test-Path $map) { $mapTxt = Get-Content $map -Raw }
        $okLog = $mapTxt.Contains('transport_capture_logger_task')
        $okBench = $mapTxt.Contains('session_send_bench_forge_next')
        Write-Host "=== LINKED $($m.name): bin=$size bytes sha256=$h map_logger=$okLog map_bench=$okBench ==="
        $results += "OK $($m.name) bin=$size sha256=$h logger=$okLog bench=$okBench"
    } else {
        Write-Host "!!! BUILD FAIL $($m.name): ceepew.bin missing"
        $results += "BUILD-FAIL $($m.name)"
    }
}
Write-Host '=== SUMMARY ==='
$results | ForEach-Object { Write-Host $_ }
