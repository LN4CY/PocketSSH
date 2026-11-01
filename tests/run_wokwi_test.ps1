# run_wokwi_test.ps1 - Run automated Wokwi simulation tests for PocketSSH
$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $repoRoot

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "   POCKETSSH WOKWI SIMULATOR AUTOMATED INTEGRATION TESTS  " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host ""

# 1. Verify build artifacts
$flasherArgs = Join-Path $repoRoot "build\flasher_args.json"
$appElf = Join-Path $repoRoot "build\PocketSSH.elf"
if (-not (Test-Path $flasherArgs) -or -not (Test-Path $appElf)) {
    Write-Error "Build artifacts not found. Please run .\build_firmware.ps1 first."
    exit 1
}

# 2. Check for wokwi-cli
$wokwiCmd = Get-Command "wokwi-cli" -ErrorAction SilentlyContinue
if (-not $wokwiCmd) {
    # Check default installer locations
    $candidatePaths = @(
        (Join-Path $env:USERPROFILE ".wokwi\wokwi-cli.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\wokwi-cli\wokwi-cli.exe"),
        (Join-Path $env:USERPROFILE "bin\wokwi-cli.exe")
    )
    foreach ($p in $candidatePaths) {
        if (Test-Path $p) {
            $wokwiCmd = $p
            break
        }
    }
}

if (-not $wokwiCmd) {
    Write-Host "[!] wokwi-cli was not found on your system." -ForegroundColor Yellow
    Write-Host "    You can install it automatically using PowerShell:" -ForegroundColor Yellow
    Write-Host "    iwr https://wokwi.com/ci/install.ps1 -useb | iex" -ForegroundColor White
    Write-Host ""
    Write-Host "    Or install via npm (if Node.js is installed):" -ForegroundColor Yellow
    Write-Host "    npm install -g @wokwi/cli" -ForegroundColor White
    Write-Host ""
    Write-Host "    Alternatively, open this folder in VS Code with the 'Wokwi Simulator' extension to run interactively." -ForegroundColor Green
    exit 1
}

# 3. Check for WOKWI_CLI_TOKEN
if (-not $env:WOKWI_CLI_TOKEN) {
    Write-Host "[!] WOKWI_CLI_TOKEN environment variable is not set." -ForegroundColor Yellow
    Write-Host "    Get your free Wokwi CI token at: https://wokwi.com/dashboard/ci" -ForegroundColor White
    Write-Host "    Then run:" -ForegroundColor White
    Write-Host "    `$env:WOKWI_CLI_TOKEN = 'wok_your_token_here'" -ForegroundColor White
    Write-Host "    .\tests\run_wokwi_test.ps1" -ForegroundColor White
    exit 1
}

# 4. Run automated scenario
$scenarioPath = Join-Path $repoRoot "tests\wokwi_scenario.yaml"
Write-Host "[*] Launching Wokwi CLI automated scenario..." -ForegroundColor Cyan
Write-Host "    Scenario: $scenarioPath" -ForegroundColor Gray
Write-Host ""

$exeName = if ($wokwiCmd -is [string]) { $wokwiCmd } else { $wokwiCmd.Source }
& $exeName . --scenario $scenarioPath --timeout 35000

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "==========================================================" -ForegroundColor Green
    Write-Host " [OK] ALL WOKWI AUTOMATED INTEGRATION TESTS PASSED!       " -ForegroundColor Green
    Write-Host "==========================================================" -ForegroundColor Green
    exit 0
} else {
    Write-Host ""
    Write-Host "==========================================================" -ForegroundColor Red
    Write-Host " [FAIL] Wokwi automated tests failed with exit code $LASTEXITCODE" -ForegroundColor Red
    Write-Host "==========================================================" -ForegroundColor Red
    exit $LASTEXITCODE
}
