<#
.SYNOPSIS
  Flash one or two ESP32-CEE-PEW devices with smart port detection,
  parallel flashing, and integrated monitoring.

.DESCRIPTION
  Automatically detects ESP32 (CP210x/Silicon Labs) serial ports, builds
  the project once, flashes both devices in parallel, then offers
  interactive monitoring via tools/ceepew_monitor.py and optional log
  analysis via tools/ceepew_log_pipeline.py.

  When called without arguments the script:
    1. Scans for ESP32 serial ports (or uses -Port1/-Port2 overrides)
    2. Builds the firmware (idf.py build)
    3. Flashes both devices in parallel
    4. Prompts to start monitoring

.PARAMETER Port1
  Override port for Device A. Disables auto-detection for this slot.

.PARAMETER Port2
  Override port for Device B. Disables auto-detection for this slot.

.PARAMETER ListPorts
  Enumerate detected ESP32 serial ports and exit.

.PARAMETER SkipBuild
  Skip the build phase; flash the existing build.

.PARAMETER FullClean
  Run idf.py fullclean before building.

.PARAMETER Retry
  Retry a failed flash once with board reset.

.PARAMETER NoMonitor
  Exit after flashing; do not prompt for monitoring.

.PARAMETER MonitorBoth
  Skip the interactive prompt; monitor both devices immediately.

.PARAMETER MonitorA
  Skip the interactive prompt; monitor only Device A.

.PARAMETER MonitorB
  Skip the interactive prompt; monitor only Device B.

.PARAMETER Duration
  Monitoring duration in seconds (default: 120). Passed to ceepew_monitor.py.

.PARAMETER Quiet
  Suppress non-essential output.

.EXAMPLE
  # Auto-detect ports, build, flash, prompt
  .\tools\flash_both.ps1

.EXAMPLE
  # Specify ports explicitly
  .\tools\flash_both.ps1 -Port1 COM5 -Port2 COM6

.EXAMPLE
  # List ports and exit
  .\tools\flash_both.ps1 -ListPorts

.EXAMPLE
  # Skip build, flash existing, monitor both for 5 minutes
  .\tools\flash_both.ps1 -SkipBuild -MonitorBoth -Duration 300

.EXAMPLE
  # Clean build, flash with retry, no monitoring
  .\tools\flash_both.ps1 -FullClean -Retry -NoMonitor

.EXAMPLE
  # Quiet mode for CI pipelines
  .\tools\flash_both.ps1 -Port1 COM5 -Port2 COM6 -SkipBuild -NoMonitor -Quiet
#>

param(
    [string]$Port1,
    [string]$Port2,
    [switch]$ListPorts,
    [switch]$SkipBuild,
    [switch]$FullClean,
    [switch]$Retry,
    [switch]$NoMonitor,
    [switch]$MonitorBoth,
    [switch]$MonitorA,
    [switch]$MonitorB,
    [int]$Duration = 120,
    [switch]$Quiet
)

# ---------------------------------------------------------------------------
# Paths and globals
# ---------------------------------------------------------------------------
$ScriptRoot    = $PSScriptRoot
$ProjectRoot   = Split-Path -Path $ScriptRoot -Parent
$LogDir        = Join-Path $ProjectRoot "logs"

# Tool & environment (matching ceepew_diagnose.ps1 conventions)
$IdfProfile    = $env:CEEPEW_IDF_PROFILE
if (-not $IdfProfile) {
    if (Test-Path -LiteralPath "C:\esp\v6.0.2\esp-idf\export.ps1") {
        $IdfProfile = "C:\esp\v6.0.2\esp-idf\export.ps1"
    } elseif (Test-Path -LiteralPath "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1") {
        $IdfProfile = "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
    } else {
        $IdfProfile = "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
    }
}
$PythonExe     = $env:CEEPEW_PYTHON
if (-not $PythonExe) {
    if (Test-Path -LiteralPath "C:\Users\Rishi Misra\.espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe") {
        $PythonExe = "C:\Users\Rishi Misra\.espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe"
    } elseif (Test-Path -LiteralPath "C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe") {
        $PythonExe = "C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe"
    } else {
        $PythonExe = "C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe"
    }
}
$MonitorPy     = Join-Path $ScriptRoot "ceepew_monitor.py"
$LogPipelinePy = Join-Path $ScriptRoot "ceepew_log_pipeline.py"
$PortCacheFile = Join-Path $LogDir ".last_ports.json"

if (-not (Test-Path -LiteralPath $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
}

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Helpers - coloured console output
# ---------------------------------------------------------------------------
function Write-Banner {
    param([string]$Msg)
    $line = "=" * 72
    Write-Host "`n$line" -ForegroundColor Cyan
    Write-Host "  $Msg" -ForegroundColor Cyan
    Write-Host "$line`n" -ForegroundColor Cyan
}

function Write-Info  { if (-not $Quiet) { Write-Host "  $args" } }
function Write-Ok    { Write-Host "  $args" -ForegroundColor Green }
function Write-Warn  { Write-Host "  $args" -ForegroundColor Yellow }
function Write-Err   { Write-Host "  $args" -ForegroundColor Red }

function Format-Elapsed {
    param([double]$Seconds)
    $ts = [TimeSpan]::FromSeconds($Seconds)
    if ($ts.Hours -gt 0)   { return "{0}h{1:mm}m{2:ss}s"  -f $ts.Hours, $ts, $ts }
    if ($ts.Minutes -gt 0)  { return "{0}m{1:ss}s" -f $ts.Minutes, $ts }
    return "${Seconds:0.0}s"
}

# ---------------------------------------------------------------------------
# Environment initialisation
# ---------------------------------------------------------------------------
function Initialize-Environment {
    if (Test-Path -LiteralPath $IdfProfile) {
        Write-Info "[env] Sourcing IDF profile: $IdfProfile"
        . $IdfProfile
    } else {
        Write-Warn "[env] IDF profile not found at $IdfProfile"
        Write-Warn "[env] Ensure ESP-IDF environment is set manually, or set CEEPEW_IDF_PROFILE"
    }

    $script:PythonExeResolved = $PythonExe
    if (-not (Get-Command $script:PythonExeResolved -ErrorAction SilentlyContinue)) {
        $fallback = (Get-Command python -ErrorAction SilentlyContinue).Source
        if ($fallback) {
            Write-Warn "[env] Python not found at configured path; using system python: $fallback"
            $script:PythonExeResolved = $fallback
        } else {
            Write-Err "[env] Python not found. Install Python 3.12+ or set CEEPEW_PYTHON"
            exit 5
        }
    }
}

# ---------------------------------------------------------------------------
# Port detection
# ---------------------------------------------------------------------------
function Find-EspPorts {
    <#
    .SYNOPSIS
      Enumerate serial ports attached to CP210x / Silicon Labs USB-UART
      bridges typical of ESP32 DevKits.
    #>
    $results = @()

    try {
        $devices = Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop
        $filtered = $devices | Where-Object {
            ($_.Name -match 'CP210|Silicon Labs|SLAB|USB Serial' -or
             $_.Description -match 'CP210|Silicon Labs|SLAB|USB Serial') -and
            $_.Name -match '\(COM\d+\)'
        }
        foreach ($d in $filtered) {
            if ($d.Name -match '\(COM(\d+)\)') {
                $results += [PSCustomObject]@{
                    Port    = "COM$($Matches[1])"
                    Name    = $d.Name
                    PortNum = [int]$Matches[1]
                }
            }
        }
    } catch {
        Write-Warn "[detect] WMI enumeration failed: $_"
    }

    # Fallback: scan registry
    if ($results.Count -eq 0) {
        try {
            $reg = Get-ItemProperty -Path "HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM" -ErrorAction Stop
            $reg.PSObject.Properties | Where-Object {
                $_.Name -match 'CP210|VCP|SLAB|Silicon'
            } | ForEach-Object {
                if ($_.Value -match '^COM(\d+)$') {
                    $results += [PSCustomObject]@{
                        Port    = $_.Value
                        Name    = $_.Name
                        PortNum = [int]$Matches[1]
                    }
                }
            }
        } catch {
            Write-Warn "[detect] Registry fallback failed: $_"
        }
    }

    return ($results | Sort-Object PortNum)
}

function Read-PortCache {
    if (Test-Path -LiteralPath $PortCacheFile) {
        try { return Get-Content -LiteralPath $PortCacheFile -Raw | ConvertFrom-Json }
        catch { }
    }
    return $null
}

function Write-PortCache {
    param([string[]]$Ports)
    try {
        $data = [PSCustomObject]@{
            Timestamp = (Get-Date -Format 'o')
            Ports     = $Ports
        }
        $data | ConvertTo-Json -Compress | Set-Content -LiteralPath $PortCacheFile -Encoding utf8 -NoNewline
    } catch { Write-Warn "[detect] Could not write port cache: $_" }
}

function Resolve-Ports {
    <#
    .SYNOPSIS
      Determine which two COM ports to use. Returns (portA, portB).
    #>

    # Both explicitly provided - override everything
    if ($Port1 -and $Port2) {
        Write-Info "[detect] Using explicit ports: $Port1 (A), $Port2 (B)"
        Write-PortCache -Ports @($Port1, $Port2)
        return ($Port1, $Port2)
    }

    $detected = Find-EspPorts
    $cache    = Read-PortCache

    # Exactly one explicit port given
    if ($Port1 -and -not $Port2) {
        $p1 = $Port1
        # Try to find the second port among detected devices
        $remaining = $detected | Where-Object { $_.Port -ne $p1 }
        if ($remaining.Count -ge 1) {
            $p2 = $remaining[0].Port
            Write-Info "[detect] $p1 explicitly set; auto-selecting $p2 as Device B"
            Write-PortCache -Ports @($p1, $p2)
            return ($p1, $p2)
        }
        # Fall through to prompt
        Write-Warn "[detect] Only $p1 is explicitly set - will prompt for Device B port"
    }

    # -- Port enumeration based on detection count --

    if ($detected.Count -eq 0 -and -not $Port1) {
        Write-Err ""
        Write-Err "No ESP32 serial ports detected."
        Write-Err "Troubleshooting:"
        Write-Err "  1. Is the ESP32 plugged in via USB?"
        Write-Err "  2. Are the CP210x drivers installed?"
        Write-Err "     https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers"
        Write-Err "  3. Try a different USB cable or port"
        if ($cache -and $cache.Ports) {
            $cached = $cache.Ports -join ', '
            Write-Err ""
            Write-Err "Last used ports: $cached"
            Write-Err "You can use -Port1 and -Port2 to specify them:"
            Write-Err "  .\tools\flash_both.ps1 -Port1 $($cache.Ports[0]) -Port2 $($cache.Ports[1])"
        }
        exit 3
    }

    Write-Info "[detect] Found $($detected.Count) ESP32 device(s):"
    if (-not $Quiet) {
        foreach ($d in $detected) {
            Write-Info "         $($d.Port) - $($d.Name)"
        }
    }

    # -- Exactly 2 detected --
    if ($detected.Count -eq 2 -and -not $Port1) {
        $ports = $detected.Port
        Write-Info "[detect] Device A -> $($ports[0]) (lower port)"
        Write-Info "[detect] Device B -> $($ports[1]) (higher port)"
        Write-PortCache -Ports @($ports[0], $ports[1])
        return ($ports[0], $ports[1])
    }

    # -- Exactly 1 detected (and no -Port1 already handled above) --
    if ($detected.Count -eq 1 -and -not $Port1) {
        $p1 = $detected[0].Port
        $suggested = $null
        if ($cache -and $cache.Ports) {
            $cachedOther = $cache.Ports | Where-Object { $_ -ne $p1 }
            if ($cachedOther) { $suggested = $cachedOther[0] }
        }

        Write-Warn "[detect] Only one ESP32 port found: $p1"
        $promptMsg = "Enter second COM port"
        if ($suggested) { $promptMsg += " (default: $suggested)" }
        $promptMsg += ": "
        $input = Read-Host $promptMsg
        $p2 = if ([string]::IsNullOrWhiteSpace($input)) { $suggested } else { $input.Trim().ToUpper() }

        if ([string]::IsNullOrWhiteSpace($p2)) {
            Write-Err "[detect] No second port provided. Use -Port2 to specify, or connect a second device."
            exit 3
        }

        Write-Info "[detect] Device A -> $p1 (auto)"
        Write-Info "[detect] Device B -> $p2 (manual)"
        Write-PortCache -Ports @($p1, $p2)
        return ($p1, $p2)
    }

    # -- 3+ detected (or 1 with -Port1 set but no second found) --
    $p1 = if ($Port1) { $Port1 } else { $detected[0].Port }

    if ($detected.Count -ge 2 -and -not $Port1) {
        Write-Host ""
        Write-Host "  Multiple ESP32 ports detected. Select two:" -ForegroundColor Yellow
        for ($i = 0; $i -lt $detected.Count; $i++) {
            $mark = ''
            if ($cache -and $cache.Ports -contains $detected[$i].Port) { $mark = ' (last used)' }
            Write-Host "  [$($i+1)] $($detected[$i].Port) - $($detected[$i].Name)$mark" -ForegroundColor Gray
        }

        $defaultA = $null
        $defaultB = $null
        if ($cache -and $cache.Ports.Count -ge 2) {
            $idxA = [array]::IndexOf($detected.Port, $cache.Ports[0])
            $idxB = [array]::IndexOf($detected.Port, $cache.Ports[1])
            if ($idxA -ge 0) { $defaultA = $idxA + 1 }
            if ($idxB -ge 0) { $defaultB = $idxB + 1 }
        }

        $selA = Read-Host "Device A port number$(if ($defaultA) { " (default: $defaultA)" } else { '' })"
        if ([string]::IsNullOrWhiteSpace($selA)) { $selA = $defaultA }

        $selB = Read-Host "Device B port number$(if ($defaultB) { " (default: $defaultB)" } else { '' })"
        if ([string]::IsNullOrWhiteSpace($selB)) { $selB = $defaultB }

        # If only one valid selection, default the other
        $idxA = if ($selA) { [int]$selA - 1 } else { -1 }
        $idxB = if ($selB) { [int]$selB - 1 } else { -1 }

        if ($idxA -lt 0 -or $idxA -ge $detected.Count) { $idxA = 0; $p1 = $detected[0].Port }
        else { $p1 = $detected[$idxA].Port }

        # Pick B: if same as A, pick next available
        if ($idxB -lt 0 -or $idxB -ge $detected.Count -or $idxB -eq $idxA) {
            $candidates = $detected | Where-Object { $_.Port -ne $p1 }
            if ($candidates.Count -ge 1) { $p2 = $candidates[0].Port }
            else { Write-Err "[detect] Could not resolve two distinct ports."; exit 3 }
        } else {
            $p2 = $detected[$idxB].Port
        }
    } else {
        # Fallback: user gave -Port1, need to pick any remaining detected port
        $remaining = $detected | Where-Object { $_.Port -ne $p1 }
        if ($remaining.Count -ge 1) {
            $p2 = $remaining[0].Port
        } else {
            Write-Err "[detect] Only one distinct ESP32 port available ($p1). Connect a second device or specify -Port2."
            exit 3
        }
    }

    Write-Info "[detect] Device A -> $p1"
    Write-Info "[detect] Device B -> $p2"
    Write-PortCache -Ports @($p1, $p2)
    return ($p1, $p2)
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
function Invoke-Build {
    if ($SkipBuild) {
        Write-Warn "[build] Skipping build (-SkipBuild)"
        return $true
    }

    Write-Banner "Building (idf.py build)"

    if ($FullClean) {
        Write-Info "[build] Running idf.py fullclean"
        idf.py fullclean 2>&1 | Out-Null
    }

    $timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $buildLog  = Join-Path $LogDir "build_${timestamp}.log"

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $output = idf.py build 2>&1
    $sw.Stop()

    $output | Out-File -FilePath $buildLog -Encoding utf8

    if ($LASTEXITCODE -ne 0) {
        Write-Err "[build] BUILD FAILED (exit $LASTEXITCODE)"
        if (-not $Quiet) { $output | ForEach-Object { Write-Host $_ } }
        Write-Err "[build] Full log: $buildLog"
        return $false
    }

    Write-Ok "[build] OK ($(Format-Elapsed $sw.Elapsed.TotalSeconds)) log: $(Split-Path $buildLog -Leaf)"
    return $true
}

# ---------------------------------------------------------------------------
# Flash - parallel via ForEach-Object -Parallel -AsJob (PowerShell 7+)
# ---------------------------------------------------------------------------
function Invoke-FlashBoth {
    param([string]$PortA, [string]$PortB)

    Write-Banner "Flashing Both Devices"

    Write-Info " Device A: $PortA"
    Write-Info " Device B: $PortB"
    Write-Info " Started:  $(Get-Date -Format 'HH:mm:ss')"

    # Source IDF env in the calling scope so background runspaces can inherit it
    if (Test-Path -LiteralPath $IdfProfile) { . $IdfProfile }

    $ports = @($PortA, $PortB)
    $flashTimer = [System.Diagnostics.Stopwatch]::StartNew()

    $job = @()
    foreach ($port in $ports) {
        Write-Info "Flashing $port..."
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            $output = idf.py -p $port flash 2>&1 | ForEach-Object { "$_" }
            $exitCode = $LASTEXITCODE
        } catch {
            $output = @("JOB ERROR: $_")
            $exitCode = -1
        }
        $sw.Stop()

        $job += [PSCustomObject]@{
            Port     = $port
            ExitCode = $exitCode
            Elapsed  = $sw.Elapsed.TotalSeconds
            Output   = $output
        }
    }

    $flashTimer.Stop()
    Write-Info ""

    # Collect and display results
    $failedPorts = @()
    $resultRows  = @()

    foreach ($r in $job) {
        $label = if ($r.Port -eq $PortA) { 'A' } else { 'B' }
        $timeStr = Format-Elapsed $r.Elapsed

        if ($r.ExitCode -eq 0) {
            Write-Ok "[$($r.Port)] OK ($timeStr)"
            $resultRows += [PSCustomObject]@{ Device = $label; Port = $r.Port; Result = "OK"; Time = $timeStr }
        } else {
            Write-Err "[$($r.Port)] FAIL (exit $($r.ExitCode), $timeStr)"
            Write-Err "        Last output lines:"
            $tail = $r.Output | Select-Object -Last 5
            foreach ($line in $tail) { Write-Err "          $line" }
            $resultRows += [PSCustomObject]@{ Device = $label; Port = $r.Port; Result = "FAIL"; Time = $timeStr }
            $failedPorts += $r.Port
        }
    }

    # Table
    Write-Host ""
    Write-Host "  +--------+----------+------------------+" -ForegroundColor Gray
    Write-Host "  | Device | Port     | Result           |" -ForegroundColor Gray
    Write-Host "  +--------+----------+------------------+" -ForegroundColor Gray
    foreach ($r in $resultRows) {
        $resultField = if ($r.Result -eq 'OK') { "OK ($($r.Time))        " } else { "FAIL               " }
        Write-Host "  | $($r.Device)      | $($r.Port)      | $resultField|" -ForegroundColor Gray
    }
    Write-Host "  +--------+----------+------------------+" -ForegroundColor Gray
    Write-Ok "Total flash time: $(Format-Elapsed $flashTimer.Elapsed.TotalSeconds)"

    # Retry failed ports
    if ($failedPorts.Count -gt 0 -and $Retry) {
        Write-Warn "[retry] Retrying failed port(s): $($failedPorts -join ', ')"
        foreach ($port in $failedPorts) {
            Write-Info "[retry] $port - resetting board via DTR/RTS..."
            try {
                $ser = New-Object System.IO.Ports.SerialPort($port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
                $ser.Open()
                $ser.DtrEnable = $false
                $ser.RtsEnable = $true
                Start-Sleep -Milliseconds 100
                $ser.RtsEnable = $false
                Start-Sleep -Milliseconds 500
                $ser.Close()
            } catch {
                Write-Warn "[retry] Serial reset failed: $_ - continuing with flash anyway"
            }

            Write-Info "[retry] $port - flashing (attempt 2)..."
            $swRetry = [System.Diagnostics.Stopwatch]::StartNew()
            try {
                $output = idf.py -p $port flash 2>&1 | ForEach-Object { "$_" }
                $retryExit = $LASTEXITCODE
            } catch {
                $output = @("JOB ERROR: $_")
                $retryExit = -1
            }
            $swRetry.Stop()

            if ($retryExit -eq 0) {
                Write-Ok "[retry] $port OK ($(Format-Elapsed $swRetry.Elapsed.TotalSeconds))"
                $failedPorts = $failedPorts | Where-Object { $_ -ne $port }
            } else {
                Write-Err "[retry] $port FAIL again (exit $retryExit)"
            }
        }
    }

    return ($failedPorts.Count -eq 0)
}

# ---------------------------------------------------------------------------
# Monitoring
# ---------------------------------------------------------------------------
function Invoke-Monitoring {
    param([string[]]$MonitorPorts)

    if ($MonitorPorts.Count -eq 0) { return }

    $portArgs = @()
    foreach ($p in $MonitorPorts) { $portArgs += "--port"; $portArgs += $p }

    $timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $logPath   = Join-Path $LogDir "flash_monitor_${timestamp}.txt"

    $extraArgs = @("--duration", $Duration, "--log", $logPath)
    if ($MonitorPorts.Count -eq 2) { $extraArgs += "--log-per-port" }

    Write-Banner "Monitoring $($MonitorPorts -join ', ') for ${Duration}s"
    Write-Info "  Log: $logPath"

    try {
        & $script:PythonExeResolved $MonitorPy @portArgs @extraArgs
        $monitorExit = $LASTEXITCODE
        if ($monitorExit -ne 0) {
            Write-Warn "[monitor] ceepew_monitor.py exited with code $monitorExit"
        }
    } catch {
        Write-Err "[monitor] Failed to launch monitor: $_"
        return
    }

    # Offer log pipeline
    if (Test-Path -LiteralPath $LogPipelinePy -and (Test-Path -LiteralPath $logPath)) {
        Write-Host ""
        $runPipeline = Read-Host "Run log analysis pipeline on captured logs? [y/N]"
        if ($runPipeline -eq 'y' -or $runPipeline -eq 'Y') {
            Write-Banner "Running Log Pipeline"
            try {
                & $script:PythonExeResolved $LogPipelinePy ingest $logPath --report
                if ($LASTEXITCODE -eq 0) {
                    Write-Ok "[pipeline] Analysis complete - see DEVICE_LOG_FINDINGS.md"
                } else {
                    Write-Err "[pipeline] Pipeline exited with code $LASTEXITCODE"
                }
            } catch {
                Write-Err "[pipeline] Failed: $_"
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
Write-Banner "CEE-PEW  flash_both"

Initialize-Environment
Set-Location -Path $ProjectRoot

# --list-ports
if ($ListPorts) {
    Write-Info "[detect] Scanning serial ports..."
    $ports = Find-EspPorts
    if ($ports.Count -eq 0) {
        Write-Host ""
        Write-Warn "No ESP32 serial ports detected."
        exit 0
    }
    Write-Host ""
    Write-Host "  Detected ESP32 ports:" -ForegroundColor Cyan
    foreach ($p in $ports) {
        Write-Host "    $($p.Port) - $($p.Name)" -ForegroundColor Gray
    }
    $cache = Read-PortCache
    if ($cache -and $cache.Ports) {
        Write-Host ""
        Write-Host "  Last used: $($cache.Ports -join ', ')" -ForegroundColor Gray
    }
    exit 0
}

# Resolve ports
$portA, $portB = Resolve-Ports

# Build
$buildOk = Invoke-Build
if (-not $buildOk) { exit 2 }

# Flash
$flashOk = Invoke-FlashBoth -PortA $portA -PortB $portB
if (-not $flashOk) {
    Write-Err "[flash] One or more devices failed to flash."
    exit 1
}

# --no-monitor
if ($NoMonitor) {
    Write-Ok "[done] Both devices flashed successfully. Exiting (-NoMonitor)."
    exit 0
}

# Interactive monitor prompt (unless overridden by flags)
$monitorChoice = $null
if ($MonitorBoth) { $monitorChoice = '1' }
elseif ($MonitorA) { $monitorChoice = '2' }
elseif ($MonitorB) { $monitorChoice = '3' }

if (-not $monitorChoice) {
    Write-Banner "Post-Flash"
    Write-Ok "Both devices flashed successfully."
    Write-Host ""
    Write-Host "  [1] Monitor both devices"
    Write-Host "  [2] Monitor only Device A ($portA)"
    Write-Host "  [3] Monitor only Device B ($portB)"
    Write-Host "  [4] Exit"
    Write-Host ""
    $input = Read-Host " Choice [1]"
    if ([string]::IsNullOrWhiteSpace($input)) { $input = '1' }
    $monitorChoice = $input
}

switch ($monitorChoice) {
    '1' { Invoke-Monitoring -MonitorPorts @($portA, $portB) }
    '2' { Invoke-Monitoring -MonitorPorts @($portA) }
    '3' { Invoke-Monitoring -MonitorPorts @($portB) }
    '4' { Write-Ok "[done] Exiting."; exit 0 }
    default { Write-Warn "Invalid choice. Exiting."; exit 0 }
}

Write-Banner "Done - DEVICE_LOG_FINDINGS.md may contain post-analysis results"
