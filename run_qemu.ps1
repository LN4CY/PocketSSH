# run_qemu.ps1 - Launch PocketSSH interactively in QEMU
$repoRoot = $PSScriptRoot
$flashBin = Join-Path $repoRoot "build\qemu_flash.bin"
$efuseBin = Join-Path $repoRoot "build\qemu_efuse.bin"

if (-not (Test-Path $flashBin)) {
    Write-Error "Flash image not found at $flashBin. Run .\build_firmware.ps1 first."
    exit 1
}

$qemuExe = "E:\proj\sdk\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe"
if (-not (Test-Path $qemuExe)) {
    $searchRoots = @(
        $env:IDF_TOOLS_PATH,
        "E:\proj\sdk\.espressif",
        (Join-Path $env:USERPROFILE ".espressif")
    ) | Where-Object { $_ -and (Test-Path $_) }

    foreach ($root in $searchRoots) {
        $found = Get-ChildItem -Path (Join-Path $root "tools\qemu-xtensa") -Filter "qemu-system-xtensa.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) {
            $qemuExe = $found.FullName
            break
        }
    }
}

if (-not (Test-Path $qemuExe)) {
    Write-Error "qemu-system-xtensa.exe not found."
    exit 1
}

Write-Host "Starting PocketSSH in QEMU (ESP32-S3)..." -ForegroundColor Cyan
Write-Host "Press Ctrl+A then X to exit QEMU." -ForegroundColor Yellow
Write-Host ""

$qemuArgs = @(
    "-machine", "esp32s3",
    "-drive", "file=$flashBin,if=mtd,format=raw",
    "-drive", "file=$efuseBin,if=none,format=raw,id=efuse",
    "-nographic",
    "-serial", "mon:stdio"
)

& $qemuExe $qemuArgs
