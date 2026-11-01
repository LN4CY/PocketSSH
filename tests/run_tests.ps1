# PocketSSH Host Verification and Validation Test Runner
param (
    [switch]$Verbose
)

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  POCKETSSH PRE-BUILD VERIFICATION AND VALIDATION TESTS" -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Cyan

$pyScript = Join-Path $PSScriptRoot "test_terminal.py"
$pythonExe = "python"

if (-not (Get-Command $pythonExe -ErrorAction SilentlyContinue)) {
    if (Test-Path "E:\proj\sdk\.espressif\tools\idf-python\3.11.2\python.exe") {
        $pythonExe = "E:\proj\sdk\.espressif\tools\idf-python\3.11.2\python.exe"
    }
}

if ($Verbose) {
    & $pythonExe $pyScript -v
} else {
    & $pythonExe $pyScript
}

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "[OK] ALL 22 SUBSYSTEM UNIT AND INTEGRATION TESTS PASSED (100% VERIFIED)" -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "[ERROR] TEST FAILURES DETECTED! HALTING FIRMWARE BUILD." -ForegroundColor Red
}

exit $LASTEXITCODE
