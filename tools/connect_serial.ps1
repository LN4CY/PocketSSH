# PocketSSH Wokwi Interactive Serial Terminal Client
$port = 4000
$hostAddress = "127.0.0.1"

# Check if Wokwi RFC2217 port is listening
$socket = New-Object System.Net.Sockets.TcpClient
try {
    $asyncResult = $socket.BeginConnect($hostAddress, $port, $null, $null)
    $success = $asyncResult.AsyncWaitHandle.WaitOne(1000, $false)
    if (-not $success -or -not $socket.Connected) {
        Write-Host "==========================================================" -ForegroundColor Yellow
        Write-Host " [!] Wokwi RFC2217 Port 4000 is not reachable." -ForegroundColor Yellow
        Write-Host "==========================================================" -ForegroundColor Yellow
        Write-Host "Please start the Wokwi simulation first:" -ForegroundColor White
        Write-Host "  1. Press F1 in VS Code" -ForegroundColor Cyan
        Write-Host "  2. Select 'Wokwi: Start Simulator'" -ForegroundColor Cyan
        Write-Host "  3. Re-run this task once the simulation has started." -ForegroundColor White
        Write-Host "==========================================================" -ForegroundColor Yellow
        exit 1
    }
} catch {
    Write-Host "Error connecting: $_" -ForegroundColor Red
    exit 1
} finally {
    $socket.Close()
}

# Locate Python environment with pyserial
$pythonPaths = @(
    "E:\proj\sdk\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe",
    "E:\proj\sdk\.espressif\python_env\*\Scripts\python.exe",
    "python.exe"
)

$pyExe = $null
foreach ($path in $pythonPaths) {
    $resolved = Resolve-Path $path -ErrorAction SilentlyContinue
    if ($resolved) {
        $pyExe = $resolved[0].Path
        break
    }
}

if (-not $pyExe) {
    $pyExe = "python"
}

Write-Host "Connecting to PocketSSH on Wokwi (rfc2217://localhost:$port)..." -ForegroundColor Green
Write-Host "Press Ctrl+] to exit the serial console." -ForegroundColor Gray
& $pyExe -m serial.tools.miniterm --raw "rfc2217://localhost:$port" 115200
