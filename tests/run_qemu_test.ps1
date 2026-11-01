# PocketSSH Virtual ESP32-S3 Target (QEMU) Integration Test Runner
param (
    [switch]$Verbose
)

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  POCKETSSH VIRTUAL TARGET (QEMU) INTEGRATION TESTS" -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Cyan

$pyScript = Join-Path $PSScriptRoot "test_qemu.py"
$pythonExe = "python"

if (-not (Get-Command $pythonExe -ErrorAction SilentlyContinue)) {
    if (Test-Path "E:\proj\sdk\.espressif\tools\idf-python\3.11.2\python.exe") {
        $pythonExe = "E:\proj\sdk\.espressif\tools\idf-python\3.11.2\python.exe"
    }
}

& $pythonExe $pyScript
exit $LASTEXITCODE
