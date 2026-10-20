<#
.SYNOPSIS
  Initialize the repository's ESP-IDF 6.0.2 environment.

.DESCRIPTION
  Clears stale ESP-IDF variables before invoking the official export script.
  This is important when another ESP-IDF installation was previously active in
  the same PowerShell session.
#>

[CmdletBinding()]
param(
    [switch]$VerifyOnly
)

$ErrorActionPreference = "Stop"

$env:IDF_PATH = "C:\esp\v6.0.3\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python\v6.0.3\venv"
$exportScript = Join-Path $env:IDF_PATH "export.ps1"

foreach ($path in @($env:IDF_PATH, $env:IDF_TOOLS_PATH, $env:IDF_PYTHON_ENV_PATH, $exportScript)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "ESP-IDF setup path does not exist: $path"
    }
}

if (-not $VerifyOnly) {
    & $exportScript
    if ($LASTEXITCODE -ne 0) {
        throw "ESP-IDF export failed with exit code $LASTEXITCODE."
    }
}

$requiredCommands = @(
    "idf.py",
    "xtensa-esp-elf-gcc",
    "riscv32-esp-elf-gcc",
    "openocd",
    "ccache",
    "dfu-util"
)

foreach ($commandName in $requiredCommands) {
    if (-not (Get-Command $commandName -ErrorAction SilentlyContinue)) {
        throw "Required ESP-IDF command is not available: $commandName"
    }
}

if (-not (Get-Command -Name "xtensa-esp-elf-gdb-*" -ErrorAction SilentlyContinue)) {
    throw "Required ESP-IDF command is not available: xtensa-esp-elf-gdb"
}

if (-not (Get-Command -Name "riscv32-esp-elf-gdb-*" -ErrorAction SilentlyContinue)) {
    throw "Required ESP-IDF command is not available: riscv32-esp-elf-gdb"
}

Write-Host "ESP-IDF 6.0.2 environment is ready." -ForegroundColor Green
