Write-Host "Running PocketSSH Pre-Build Verification and Validation Tests..." -ForegroundColor Cyan
& powershell -ExecutionPolicy Bypass -File .\tests\run_tests.ps1
if ($LASTEXITCODE -ne 0) {
    Write-Error "Pre-build tests failed! Aborting firmware build to prevent regression."
    exit $LASTEXITCODE
}
Write-Host "All tests passed! Proceeding with firmware compilation..." -ForegroundColor Green

$env:IDF_TOOLS_PATH = "E:\proj\sdk\.espressif"
$env:PATH = "E:\proj\sdk\.espressif\tools\idf-python\3.11.2;$env:PATH"
. E:\proj\sdk\v5.4.1\esp-idf\export.ps1
idf.py build
if ($LASTEXITCODE -eq 0) {
    esptool.py --chip esp32s3 merge_bin -o build/PocketSSH_merged_0x0.bin --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/PocketSSH.bin
}
