param(
    [string]$Port1 = "COM5",
    [string]$Port2 = "COM6",
    [int]$Baud = 115200,
    [int]$Duration = 120,
    [string]$LogDir = ".\logs"
)

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$log1 = Join-Path $LogDir "dual_${Port1}_${timestamp}.log"
$log2 = Join-Path $LogDir "dual_${Port2}_${timestamp}.log"

Write-Host "Capturing from $Port1 → $log1"
Write-Host "Capturing from $Port2 → $log2"
Write-Host "Duration: ${Duration}s"

$ps1 = Start-Process -NoNewWindow -PassThru -FilePath "powershell" -ArgumentList "-Command", "& { idf.py monitor -p $Port1 -b $Baud | ForEach-Object { `$ts = '{0:F1}' -f (([datetime]::UtcNow - (Get-Date '1970-01-01')).TotalSeconds); `$line = `$_.Trim(); if (`$line -ne '') { Write-Output (''{0} [{1}] {2}'' -f `$ts, `$Port1, `$line) } } }"
#Simp ler approach - use com port reading

# Clean up old job if any
Get-Job -Name "SerialCapture1" -ErrorAction SilentlyContinue | Remove-Job
Get-Job -Name "SerialCapture2" -ErrorAction SilentlyContinue | Remove-Job

# Use .NET serial port directly
$job1 = Start-Job -Name "SerialCapture1" -ScriptBlock {
    param($port, $baud, $logFile)
    $ts = New-Object System.IO.Ports.SerialPort $port, $baud, None, 8, One
    $ts.ReadTimeout = 5000
    $stream = [System.IO.StreamWriter]::new($logFile)
    try {
        $ts.Open()
        $start = [datetime]::UtcNow
        $buf = New-Object char[] 1
        $line = ""
        while (($elapsed = ([datetime]::UtcNow - $start).TotalSeconds) -lt 130) {
            try {
                $bytesRead = $ts.Read($buf, 0, 1)
                if ($bytesRead -gt 0) {
                    $ch = $buf[0]
                    if ($ch -eq "`n") {
                        if ($line.Length -gt 0) {
                            $tsSec = '{0:F1}' -f (([datetime]::UtcNow - $start).TotalSeconds)
                            $outLine = "  $tsSec [$port] $line"
                            $stream.WriteLine($outLine)
                            $line = ""
                        }
                    } elseif ($ch -ne "`r") {
                        $line += $ch
                    }
                }
            } catch {
                # timeout
            }
        }
    } finally {
        if ($ts.IsOpen) { $ts.Close() }
        $stream.Close()
    }
} -ArgumentList $Port1, $Baud, $log1

$job2 = Start-Job -Name "SerialCapture2" -ScriptBlock {
    param($port, $baud, $logFile)
    $ts = New-Object System.IO.Ports.SerialPort $port, $baud, None, 8, One
    $ts.ReadTimeout = 5000
    $stream = [System.IO.StreamWriter]::new($logFile)
    try {
        $ts.Open()
        $start = [datetime]::UtcNow
        $buf = New-Object char[] 1
        $line = ""
        while (($elapsed = ([datetime]::UtcNow - $start).TotalSeconds) -lt 130) {
            try {
                $bytesRead = $ts.Read($buf, 0, 1)
                if ($bytesRead -gt 0) {
                    $ch = $buf[0]
                    if ($ch -eq "`n") {
                        if ($line.Length -gt 0) {
                            $tsSec = '{0:F1}' -f (([datetime]::UtcNow - $start).TotalSeconds)
                            $outLine = "  $tsSec [$port] $line"
                            $stream.WriteLine($outLine)
                            $line = ""
                        }
                    } elseif ($ch -ne "`r") {
                        $line += $ch
                    }
                }
            } catch {
                # timeout
            }
        }
    } finally {
        if ($ts.IsOpen) { $ts.Close() }
        $stream.Close()
    }
} -ArgumentList $Port2, $Baud, $log2

Start-Sleep -Seconds $Duration

$job1 | Wait-Job -Timeout 30 | Out-Null
$job2 | Wait-Job -Timeout 30 | Out-Null

Write-Host "Capture complete."
Write-Host "  $log1"
Write-Host "  $log2"
