# diagnose.ps1 - CEE-PEW On-Device Diagnostic Driver
#
# Workflow:
#   1. Open the IDF env profile (Microsoft.v6.0.1.PowerShell_profile.ps1).
#   2. Temporarily enable CEEPEW_BUILD_TESTS in sdkconfig.
#   3. Build, flash, and start idf.py monitor.
#   4. Restore sdkconfig so subsequent `idf.py build' does NOT include tests.
#   5. Watch for the "=== DIAGNOSTIC REPORT ===" block in the serial
#      log. Grep each line for PASS/FAIL.
#   6. Exit with a non-zero status if any subsystem reports FAIL.
#      Otherwise exit 0.
#
# Usage:
#   . C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1
#   .\diagnose.ps1 -Port COM5 -TimeoutSec 60
#
# The script intentionally does NOT call idf.py erase-flash or fullclean;
# it assumes a known-good boot state. Add -FullClean to wipe the build
# directory first.
#
# Sibling scripts in tools/ (e.g. verify_handoff_fix.ps1) follow the
# same source-the-IDF-profile convention; see tools/README.md.

[CmdletBinding()]
param(
    [string] $Port = 'COM5',
    [int]    $TimeoutSec = 60,
    [switch] $FullClean,
    [string] $IdfProfile = 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
)

$ErrorActionPreference = 'Continue'

# -- Step 1: source the IDF profile ---------------------------------
if (Test-Path -LiteralPath $IdfProfile) {
    Write-Host "[diagnose] Sourcing IDF profile: $IdfProfile"
    . $IdfProfile
} else {
    Write-Warning "[diagnose] IDF profile not found at $IdfProfile; assuming env is already set."
}

# -- Step 2: enable Diagnostic Mode in sdkconfig -------------------
$sdkconfig = 'sdkconfig'
if (-not (Test-Path -LiteralPath $sdkconfig)) {
    Write-Error "[diagnose] No sdkconfig in $(Get-Location); run 'idf.py menuconfig' once first."
    exit 2
}

$sc = Get-Content -LiteralPath $sdkconfig -Raw
$tests_already_enabled = $sc -match '(?m)^CONFIG_CEEPEW_BUILD_TESTS=y$'
if (-not $tests_already_enabled) {
    Write-Host "[diagnose] Enabling CONFIG_CEEPEW_BUILD_TESTS=y in sdkconfig"
    if ($sc -match '(?m)^# CONFIG_CEEPEW_BUILD_TESTS is not set$') {
        $sc = $sc -replace '(?m)^# CONFIG_CEEPEW_BUILD_TESTS is not set$', 'CONFIG_CEEPEW_BUILD_TESTS=y'
    } else {
        $sc = $sc + "`nCONFIG_CEEPEW_BUILD_TESTS=y`n"
    }
    Set-Content -LiteralPath $sdkconfig -Value $sc -NoNewline
} else {
    Write-Host "[diagnose] CONFIG_CEEPEW_BUILD_TESTS already enabled"
}

# -- Build / flash / monitor — always restore sdkconfig afterwards -
$exitCode = 0
try {
    # -- Step 3: optional fullclean --------------------------------
    if ($FullClean) {
        Write-Host "[diagnose] Running idf.py fullclean"
        idf.py fullclean | Out-Null
    }

    # -- Step 4: build ---------------------------------------------
    Write-Host "[diagnose] idf.py build"
    $build = idf.py build 2>&1
    $build | Select-String -Pattern 'error|Error' -SimpleMatch | ForEach-Object {
        Write-Warning "[diagnose] build issue: $_"
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[diagnose] idf.py build failed (exit $LASTEXITCODE)"
        $exitCode = $LASTEXITCODE
        return
    }

    # -- Step 5: flash ---------------------------------------------
    Write-Host "[diagnose] idf.py flash -p $Port"
    idf.py flash -p $Port | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[diagnose] idf.py flash failed (exit $LASTEXITCODE)"
        $exitCode = $LASTEXITCODE
        return
    }

    # -- Step 6: monitor and parse the DIAGNOSTIC REPORT -----------
    Write-Host "[diagnose] Running serial monitor on $Port (timeout $TimeoutSec s)"
    $monitorLog = Join-Path -Path $env:TEMP -ChildPath "ceepew_diag_$(Get-Date -Format yyyyMMdd_HHmmss).log"
    & "C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe" "tools/serial_monitor.py" $Port $TimeoutSec $monitorLog

    $report = $null
    if (Test-Path -LiteralPath $monitorLog) {
        $content = Get-Content -LiteralPath $monitorLog -Raw -ErrorAction SilentlyContinue
        if ($content -match '=== DIAGNOSTIC REPORT ===[\s\S]*?=========================') {
            $report = $matches[0]
        }
    }

    if ($null -eq $report) {
        Write-Error "[diagnose] Error: DIAGNOSTIC REPORT not seen or incomplete"
        $exitCode = 3
        return
    }

    # -- Step 7: parse and report ----------------------------------
    Write-Host ""
    Write-Host "---- DIAGNOSTIC REPORT ----"
    Write-Host $report
    Write-Host "---------------------------"

    $lines  = ($report -split "`n") | ForEach-Object { $_.Trim() }
    $rows   = $lines | Where-Object { $_ -match '^\[\S+' }
    $passes = ($rows | Where-Object { $_ -match 'PASS$' }).Count
    $fails  = ($rows | Where-Object { $_ -match 'FAIL$' })
    $failCount = $fails.Count

    Write-Host "[diagnose] PASS=$passes  FAIL=$failCount"
    if ($failCount -gt 0) {
        Write-Host "[diagnose] FAILED SUBSYSTEMS:"
        foreach ($f in $fails) { Write-Host "  $f" }
        $exitCode = 1
    }
} finally {
    # -- Always restore sdkconfig — even on error or Ctrl-C --------
    if (-not $tests_already_enabled) {
        Write-Host "[diagnose] Restoring sdkconfig — disabling CEEPEW_BUILD_TESTS"
        $sc = Get-Content -LiteralPath $sdkconfig -Raw
        if ($sc -match '(?m)^CONFIG_CEEPEW_BUILD_TESTS=y$') {
            $sc = $sc -replace '(?m)^CONFIG_CEEPEW_BUILD_TESTS=y$', '# CONFIG_CEEPEW_BUILD_TESTS is not set'
            Set-Content -LiteralPath $sdkconfig -Value $sc -NoNewline
            Write-Host "[diagnose] sdkconfig restored — subsequent 'idf.py build' will NOT include tests"
        } else {
            Write-Host "[diagnose] CONFIG_CEEPEW_BUILD_TESTS already absent; no restore needed"
        }
    } else {
        Write-Host "[diagnose] Tests were already enabled before diagnose; leaving sdkconfig as-is"
    }
}

exit $exitCode
