<#
.SYNOPSIS
  Unified CEE-PEW test and diagnostic driver.

.DESCRIPTION
  Consolidates the functionality of the old diagnose.ps1, run_pairing_test.ps1,
  and verify_handoff_fix.ps1 into a single script with a -Mode switch.

.PARAMETER Mode
  Which workflow to run:
    Diagnose  — Single-device on-device diagnostic.
                Enables CONFIG_CEEPEW_DEVELOPMENT_MODE, builds, flashes,
                monitors for the DIAGNOSTIC REPORT, parses PASS/FAIL results,
                then restores sdkconfig.
    Pairing   — Two-device pairing test.
                Builds, flashes both COM5/COM6, monitors both ports via
                ceepew_monitor.py, extracts key events to a CSV summary.
    Build     — Build only; prints manual pairing-test instructions.

.PARAMETER Port
  Serial port for Diagnose mode (default: COM5).

.PARAMETER Duration
  Monitoring duration in seconds (default: 120). Used in Diagnose and Pairing.

.PARAMETER TimeoutSec
  Alias for Duration (for backward compatibility with old diagnose.ps1).

.PARAMETER SkipFlash
  (Pairing) Skip the build & flash steps; just monitor.

.PARAMETER MonitorOnly
  (Pairing) Skip build, flash, and just monitor with existing firmware.

.PARAMETER NoBuild
  (Pairing) Skip build, flash existing build + monitor.

.PARAMETER FullClean
  (Diagnose) Run idf.py fullclean before building.

.PARAMETER ForceFlash
  (Build) Run build + flash both devices and exit.

.PARAMETER ForceMonitor
  (Build) Run build + flash + monitor a single device.

.EXAMPLE
  .\tools\ceepew_diagnose.ps1 -Mode Diagnose
  .\tools\ceepew_diagnose.ps1 -Mode Pairing -Duration 300
  .\tools\ceepew_diagnose.ps1 -Mode Pairing -MonitorOnly
  .\tools\ceepew_diagnose.ps1 -Mode Build
#>

param(
    [ValidateSet("Diagnose", "Pairing", "Build")]
    [string]$Mode = "Diagnose",

    [string]$Port = "COM5",
    [int]$Duration = 120,
    [int]$TimeoutSec = -1,

    [switch]$SkipFlash,
    [switch]$MonitorOnly,
    [switch]$NoBuild,
    [switch]$FullClean,
    [switch]$ForceFlash,
    [switch]$ForceMonitor
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

$IdfProfile = "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
$PythonExe  = "C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe"
$MonitorPy  = Join-Path $PSScriptRoot "ceepew_monitor.py"

# -- Fallback: if TimeoutSec was passed (old diagnose.ps1 convention), use it
if ($TimeoutSec -gt 0) { $Duration = $TimeoutSec }


# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------
function Write-Banner {
    param([string]$Msg)
    $line = "=" * 70
    Write-Host "`n$line" -ForegroundColor Cyan
    Write-Host "  $Msg" -ForegroundColor Cyan
    Write-Host "$line`n" -ForegroundColor Cyan
}

function Step-IdfBuild {
    Write-Banner "Building"
    $buildLog = Join-Path $ProjectRoot "logs" "build_$(Get-Date -Format 'yyyyMMdd_HHmmss').log"
    if (-not (Test-Path (Split-Path $buildLog -Parent))) {
        New-Item -ItemType Directory -Path (Split-Path $buildLog -Parent) | Out-Null
    }
    $output = idf.py build 2>&1
    $output | Out-File -FilePath $buildLog -Encoding utf8
    $output | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "BUILD FAILED (exit $LASTEXITCODE) — see $buildLog"
        exit 1
    }
    Write-Host "Build OK — log: $buildLog" -ForegroundColor Green
}

function Step-Flash {
    param([string]$TargetPort)
    Write-Banner "Flashing $TargetPort"
    idf.py flash -p $TargetPort 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Flash failed on $TargetPort"
        exit 1
    }
    Write-Host "$TargetPort flashed OK" -ForegroundColor Green
}

function Start-SdkconfigToggle {
    <#
    .SYNOPSIS
      Enable CONFIG_CEEPEW_DEVELOPMENT_MODE in sdkconfig.
      Returns $true if the config was already enabled.
    #>
    $sdkconfig = Join-Path $ProjectRoot "sdkconfig"
    if (-not (Test-Path -LiteralPath $sdkconfig)) {
        Write-Error "No sdkconfig in $ProjectRoot; run 'idf.py menuconfig' once first."
        exit 2
    }
    $sc = Get-Content -LiteralPath $sdkconfig -Raw
    $alreadyEnabled = $sc -match '(?m)^CONFIG_CEEPEW_DEVELOPMENT_MODE=y$'
    if (-not $alreadyEnabled) {
        Write-Host "[diagnose] Enabling CONFIG_CEEPEW_DEVELOPMENT_MODE=y in sdkconfig"
        if ($sc -match '(?m)^# CONFIG_CEEPEW_DEVELOPMENT_MODE is not set$') {
            $sc = $sc -replace '(?m)^# CONFIG_CEEPEW_DEVELOPMENT_MODE is not set$', 'CONFIG_CEEPEW_DEVELOPMENT_MODE=y'
        } else {
            $sc = $sc + "`nCONFIG_CEEPEW_DEVELOPMENT_MODE=y`n"
        }
        Set-Content -LiteralPath $sdkconfig -Value $sc -NoNewline
    } else {
        Write-Host "[diagnose] CONFIG_CEEPEW_DEVELOPMENT_MODE already enabled"
    }
    return $alreadyEnabled
}

function Stop-SdkconfigToggle {
    param([bool]$WasAlreadyEnabled)
    if ($WasAlreadyEnabled) {
        Write-Host "[diagnose] Tests were already enabled before diagnose; leaving sdkconfig as-is"
        return
    }
    $sdkconfig = Join-Path $ProjectRoot "sdkconfig"
    Write-Host "[diagnose] Restoring sdkconfig — disabling CEEPEW_DEVELOPMENT_MODE"
    $sc = Get-Content -LiteralPath $sdkconfig -Raw
    if ($sc -match '(?m)^CONFIG_CEEPEW_DEVELOPMENT_MODE=y$') {
        $sc = $sc -replace '(?m)^CONFIG_CEEPEW_DEVELOPMENT_MODE=y$', '# CONFIG_CEEPEW_DEVELOPMENT_MODE is not set'
        Set-Content -LiteralPath $sdkconfig -Value $sc -NoNewline
        Write-Host "[diagnose] sdkconfig restored"
    } else {
        Write-Host "[diagnose] CONFIG_CEEPEW_DEVELOPMENT_MODE already absent; no restore needed"
    }
}

function Invoke-CeepewMonitor {
    param(
        [string[]]$Ports,
        [int]$DurationSec,
        [string]$LogPath,
        [switch]$WatchDiag,
        [switch]$LogPerPort
    )
    $argsList = @()
    foreach ($p in $Ports) { $argsList += "--port"; $argsList += $p }
    $argsList += "--duration"; $argsList += $DurationSec
    if ($LogPath) { $argsList += "--log"; $argsList += $LogPath }
    if ($WatchDiag) { $argsList += "--watch-diag" }
    if ($LogPerPort) { $argsList += "--log-per-port" }

    Write-Host "[ceepew_monitor] $PythonExe $MonitorPy $($argsList -join ' ')" -ForegroundColor Gray
    & $PythonExe $MonitorPy @argsList
    if ($LASTEXITCODE -ne 0 -and -not $WatchDiag) {
        Write-Error "ceepew_monitor.py exited with code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

function Format-Elapsed {
    param([int]$Seconds)
    $ts = [TimeSpan]::FromSeconds($Seconds)
    if ($ts.Hours -gt 0) {
        return "{0}h{1:mm}m{2:ss}s" -f $ts.Hours, $ts, $ts
    }
    if ($ts.Minutes -gt 0) {
        return "{0}m{1:ss}s" -f $ts.Minutes, $ts
    }
    return "${Seconds}s"
}


# ---------------------------------------------------------------------------
# Mode-specific handlers
# ---------------------------------------------------------------------------
function Invoke-DiagnoseMode {
    $logDir = Join-Path $ProjectRoot "logs"
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $diagLog = Join-Path $logDir "diagnose_${timestamp}.log"

    # Source the IDF profile
    if (Test-Path -LiteralPath $IdfProfile) {
        Write-Host "[diagnose] Sourcing IDF profile: $IdfProfile"
        . $IdfProfile
    } else {
        Write-Warning "[diagnose] IDF profile not found; assuming env is already set."
    }

    Set-Location -Path $ProjectRoot

    # Enable diagnostic mode in sdkconfig
    $wasAlreadyEnabled = Start-SdkconfigToggle

    $exitCode = 0
    try {
        # Optional fullclean
        if ($FullClean) {
            Write-Host "[diagnose] Running idf.py fullclean"
            idf.py fullclean | Out-Null
        }

        # Build
        Step-IdfBuild

        # Flash
        Step-Flash -TargetPort $Port

        # Monitor with diagnostic detection
        Write-Banner "Monitoring $Port for DIAGNOSTIC REPORT (${Duration}s timeout)"
        Invoke-CeepewMonitor -Ports @($Port) -DurationSec $Duration -LogPath $diagLog -WatchDiag

        # Parse results
        if (Test-Path -LiteralPath $diagLog) {
            $content = Get-Content -LiteralPath $diagLog -Raw -ErrorAction SilentlyContinue
            if ($content -match '=== DIAGNOSTIC REPORT ===[\s\S]*?=========================') {
                $report = $matches[0]
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
                    Write-Host "[diagnose] FAILED SUBSYSTEMS:" -ForegroundColor Red
                    foreach ($f in $fails) { Write-Host "  $f" -ForegroundColor Red }
                    $exitCode = 1
                } elseif ($report -match 'DIAGNOSTIC REPORT') {
                    Write-Host "[diagnose] All subsystems PASS" -ForegroundColor Green
                }
            } else {
                Write-Error "[diagnose] DIAGNOSTIC REPORT not seen or incomplete"
                $exitCode = 3
            }
        } else {
            Write-Error "[diagnose] Monitor log not found at $diagLog"
            $exitCode = 3
        }
    } finally {
        Stop-SdkconfigToggle -WasAlreadyEnabled $wasAlreadyEnabled
    }

    Write-Banner "DIAGNOSTIC $(if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' })"
    exit $exitCode
}

function Invoke-PairingMode {
    $logDir = Join-Path $ProjectRoot "logs"
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $pairingLog = Join-Path $logDir "pairing_${timestamp}.txt"
    $summaryCsv = Join-Path $logDir "summary_${timestamp}.csv"

    # Source the IDF profile
    if (Test-Path -LiteralPath $IdfProfile) {
        Write-Host "[pairing] Sourcing IDF profile: $IdfProfile"
        . $IdfProfile
    } else {
        Write-Warning "[pairing] IDF profile not found; assuming env is already set."
    }

    Set-Location -Path $ProjectRoot

    # Step 1: Build (unless skipped)
    if (-not $MonitorOnly -and -not $SkipFlash -and -not $NoBuild) {
        Step-IdfBuild
    } elseif ($NoBuild) {
        Write-Host "[pairing] Skipping build (existing build will be flashed)" -ForegroundColor Yellow
    } elseif ($MonitorOnly) {
        Write-Host "[pairing] Skipping build and flash (MonitorOnly mode)" -ForegroundColor Yellow
    } else {
        Write-Host "[pairing] Skipping build (SkipFlash mode)" -ForegroundColor Yellow
    }

    # Step 2: Flash both devices (unless skipped)
    if (-not $MonitorOnly -and -not $SkipFlash) {
        Step-Flash -TargetPort "COM5"
        Step-Flash -TargetPort "COM6"
    }

    # Step 3: Monitor both ports
    Write-Banner "Monitoring COM5 + COM6 for ${Duration}s (log: $pairingLog)"
    Invoke-CeepewMonitor -Ports @("COM5", "COM6") -DurationSec $Duration -LogPath $pairingLog -LogPerPort

    # Step 4: Extract summary events
    Write-Banner "Extracting key events"
    $com5Log = Join-Path $logDir "com5_${timestamp}.log"
    $com6Log = Join-Path $logDir "com6_${timestamp}.log"

    function Extract-Events {
        param([string]$Path, [string]$Label)
        if (-not (Test-Path $Path)) { return @() }
        $content = Get-Content $Path -Encoding utf8
        $result = @()
        foreach ($line in $content) {
            if ($line -match '\[MILESTONE\]') { $result += "$Label,MILESTONE,$line" }
            if ($line -match '\[PASS\]')      { $result += "$Label,PASS,$line" }
            if ($line -match '\[FAIL\]')      { $result += "$Label,FAIL,$line" }
            if ($line -match 'PAIRING FAILED|PAIR_FAIL') { $result += "$Label,PAIR_FAIL,$line" }
            if ($line -match 'Phase 2 ready') { $result += "$Label,PHASE2_READY,$line" }
            if ($line -match 'PHASE3|sync_barrier_cleared') { $result += "$Label,PHASE3,$line" }
            if ($line -match 'espnow_send_cb') { $result += "$Label,ESPNOW_SEND,$line" }
            if ($line -match 'RADIO-RX')       { $result += "$Label,RADIO_RX,$line" }
            if ($line -match 'Post-derive')    { $result += "$Label,POST_DERIVE,$line" }
        }
        return $result
    }

    $events = @()
    $events += Extract-Events $com5Log "DEVICE_A"
    $events += Extract-Events $com6Log "DEVICE_B"
    $events | Sort-Object | Out-File -FilePath $summaryCsv -Encoding utf8

    Write-Host "Summary written to: $summaryCsv" -ForegroundColor Green
    Write-Host "  Combined log: $pairingLog" -ForegroundColor Green
    if (Test-Path $com5Log) { Write-Host "  Per-port log: $com5Log" -ForegroundColor Green }
    if (Test-Path $com6Log) { Write-Host "  Per-port log: $com6Log" -ForegroundColor Green }

    # Print summary
    Write-Banner "KEY EVENTS"
    $milestones = $events | Where-Object { $_ -match "MILESTONE" }
    if ($milestones) {
        $milestones | ForEach-Object { Write-Host "  $_" -ForegroundColor Cyan }
    }
    $failures = $events | Where-Object { $_ -match "FAIL|PAIR_FAIL" }
    if ($failures) {
        $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }
    $passes = $events | Where-Object { $_ -match "PASS" }
    if ($passes) {
        $passes | ForEach-Object { Write-Host "  $_" -ForegroundColor Green }
    }

    Write-Banner "PAIRING TEST COMPLETE"
}

function Invoke-BuildMode {
    # Source the IDF profile
    if (Test-Path -LiteralPath $IdfProfile) {
        Write-Host "[build] Sourcing IDF profile: $IdfProfile"
        . $IdfProfile
    } else {
        Write-Warning "[build] IDF profile not found; assuming env is already set."
    }

    Set-Location -Path $ProjectRoot

    Step-IdfBuild

    if ($ForceFlash) {
        Step-Flash -TargetPort "COM5"
        Step-Flash -TargetPort "COM6"
    } elseif ($ForceMonitor) {
        Step-Flash -TargetPort $Port
        Invoke-CeepewMonitor -Ports @($Port) -DurationSec $Duration
    } else {
        # Print manual pairing test instructions (from old verify_handoff_fix.ps1)
        Write-Banner "Manual pairing tests to perform"
        Write-Host ""
        Write-Host "1. MISMATCH test: type DIFFERENT codes on the two units." -ForegroundColor Yellow
        Write-Host "   EXPECT: BOTH units transition to PAIRING_FAILED, neither enters CHAT." -ForegroundColor Yellow
        Write-Host ""
        Write-Host "2. MATCH test: type the SAME code on both units." -ForegroundColor Yellow
        Write-Host "   EXPECT: BOTH units enter CHAT within ~500ms of last commit." -ForegroundColor Yellow
        Write-Host ""
        Write-Host "3. Watch monitor for queue_timeouts (should be rare) and the new" -ForegroundColor Yellow
        Write-Host "   'Initiator: GATTC write ACK received — awaiting peer verification result'" -ForegroundColor Yellow
        Write-Host "   log line (replaces the old handoff_ready set on write ACK line)." -ForegroundColor Yellow
        Write-Host ""

        Write-Host "Quick toggles:" -ForegroundColor Cyan
        Write-Host "  .\tools\ceepew_diagnose.ps1 -Mode Build -ForceFlash   # build + flash both" -ForegroundColor Gray
        Write-Host "  .\tools\ceepew_diagnose.ps1 -Mode Build -ForceMonitor # build + flash + monitor single" -ForegroundColor Gray
        Write-Host "  .\tools\ceepew_diagnose.ps1 -Mode Diagnose            # run on-device diagnostics" -ForegroundColor Gray
        Write-Host "  .\tools\ceepew_diagnose.ps1 -Mode Pairing             # run pairing test" -ForegroundColor Gray
    }
}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
switch ($Mode) {
    "Diagnose" { Invoke-DiagnoseMode }
    "Pairing"  { Invoke-PairingMode }
    "Build"    { Invoke-BuildMode }
}
