param(
    [string]$Port1 = "COM5",
    [string]$Port2 = "COM6",
    [int]$Baud = 115200,
    [int]$Duration = 130,
    [string]$LogDir = ".\logs"
)

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$logFile1 = Join-Path $LogDir "capture_${Port1}_${timestamp}.txt"
$logFile2 = Join-Path $LogDir "capture_${Port2}_${timestamp}.txt"

Write-Host "Capturing from ${Port1} -> ${logFile1}"
Write-Host "Capturing from ${Port2} -> ${logFile2}"
Write-Host "Duration: ${Duration}s"

$sp1 = New-Object System.IO.Ports.SerialPort $Port1, $Baud, None, 8, One
$sp1.ReadTimeout = 1000
$sp2 = New-Object System.IO.Ports.SerialPort $Port2, $Baud, None, 8, One
$sp2.ReadTimeout = 1000

$sw1 = [System.IO.StreamWriter]::new($logFile1)
$sw2 = [System.IO.StreamWriter]::new($logFile2)

try {
    $sp1.Open()
    $sp2.Open()

    $start = [datetime]::UtcNow
    $buf1 = New-Object char[] 1
    $buf2 = New-Object char[] 1
    $line1 = ""
    $line2 = ""

    while (([datetime]::UtcNow - $start).TotalSeconds -lt $Duration) {
        # Read from port 1
        try {
            while ($sp1.BytesToRead -gt 0) {
                $n = $sp1.Read($buf1, 0, 1)
                if ($n -gt 0) {
                    $ch = $buf1[0]
                    if ($ch -eq "`n") {
                        if ($line1.Length -gt 0 -or $true) {
                            $elapsed = ([datetime]::UtcNow - $start).TotalSeconds
                            $ts = "{0:F1}" -f $elapsed
                            $sw1.WriteLine("$ts [$Port1] $line1".TrimEnd())
                            $line1 = ""
                        }
                    } elseif ($ch -ne "`r") {
                        $line1 += $ch
                    }
                }
            }
        } catch { }

        # Read from port 2
        try {
            while ($sp2.BytesToRead -gt 0) {
                $n = $sp2.Read($buf2, 0, 1)
                if ($n -gt 0) {
                    $ch = $buf2[0]
                    if ($ch -eq "`n") {
                        if ($line2.Length -gt 0 -or $true) {
                            $elapsed = ([datetime]::UtcNow - $start).TotalSeconds
                            $ts = "{0:F1}" -f $elapsed
                            $sw2.WriteLine("$ts [$Port2] $line2".TrimEnd())
                            $line2 = ""
                        }
                    } elseif ($ch -ne "`r") {
                        $line2 += $ch
                    }
                }
            }
        } catch { }

        Start-Sleep -Milliseconds 10
    }

    # Flush remaining lines
    if ($line1.Length -gt 0) {
        $elapsed = ([datetime]::UtcNow - $start).TotalSeconds
        $ts = "{0:F1}" -f $elapsed
        $sw1.WriteLine("$ts [$Port1] $line1".TrimEnd())
    }
    if ($line2.Length -gt 0) {
        $elapsed = ([datetime]::UtcNow - $start).TotalSeconds
        $ts = "{0:F1}" -f $elapsed
        $sw2.WriteLine("$ts [$Port2] $line2".TrimEnd())
    }
}
finally {
    if ($sp1.IsOpen) { $sp1.Close() }
    if ($sp2.IsOpen) { $sp2.Close() }
    $sw1.Close()
    $sw2.Close()
}

Write-Host "Capture complete."
Write-Host "  $logFile1"
Write-Host "  $logFile2"
