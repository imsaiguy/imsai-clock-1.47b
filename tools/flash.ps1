# Builds the clock, flashes it and sets its time from this PC right afterwards.
# Usage (any PowerShell):  powershell -ExecutionPolicy Bypass -File tools\flash.ps1 [-Port COM9] [-NoBuild]
# The board has no battery-backed clock: it keeps the time only while it has power (and across plain resets, if the
# chip retains it). After a power cut, run  tools\set_time.py COM9  again (or this script).
param([string]$Port = "COM9", [switch]$NoBuild)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
# Assumes ESP-IDF v5.5 at %USERPROFILE%\esp\esp-idf with its tools in %USERPROFILE%\.espressif (adjust here if not).
$env:IDF_TOOLS_PATH = "$env:USERPROFILE\.espressif"
. "$env:USERPROFILE\esp\esp-idf\export.ps1" | Out-Null
Set-Location $root
if (-not $NoBuild) { idf.py build; if ($LASTEXITCODE) { throw "build failed" } }
idf.py -p $Port flash
if ($LASTEXITCODE) { throw "flash failed" }
$py = "$env:USERPROFILE\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
for ($i = 0; $i -lt 10; $i++) {            # the board restarts after flashing; wait for its USB port to come back
    Start-Sleep -Seconds 2
    & $py "$PSScriptRoot\set_time.py" $Port
    if ($LASTEXITCODE -eq 0) { break }
}
