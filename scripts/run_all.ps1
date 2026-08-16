# Script to run Broker, Consumer, and Producer in separate terminals on Windows

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "..\build\windows-debug"

$BrokerExe = Join-Path $BuildDir "broker.exe"
$ConsumerExe = Join-Path $BuildDir "consumer.exe"
$ProducerExe = Join-Path $BuildDir "producer.exe"

# Check if executables exist
if (-not (Test-Path $BrokerExe) -or -not (Test-Path $ConsumerExe) -or -not (Test-Path $ProducerExe)) {
    Write-Error "One or more executables not found in $BuildDir. Please run scripts/build.ps1 first."
    exit 1
}

Write-Host "Starting Pub/Sub System in separate terminals..." -ForegroundColor Cyan

# 1. Start Broker
Write-Host "Launching Broker..."
Start-Process powershell -ArgumentList "-NoExit", "-Command", "& '$BrokerExe' 9000; Read-Host 'Press Enter to exit'" -WindowStyle Normal
Start-Sleep -Seconds 1

# 2. Start Consumer
Write-Host "Launching Consumer (BTC_USD)..."
Start-Process powershell -ArgumentList "-NoExit", "-Command", "& '$ConsumerExe' 127.0.0.1 9000 BTC_USD; Read-Host 'Press Enter to exit'" -WindowStyle Normal
Start-Sleep -Seconds 1

# 3. Start Producer
Write-Host "Launching Producer (BTC_USD)..."
Start-Process powershell -ArgumentList "-NoExit", "-Command", "& '$ProducerExe' 127.0.0.1 9000 BTC_USD; Read-Host 'Press Enter to exit'" -WindowStyle Normal

Write-Host "All nodes started! Look for the separate PowerShell windows." -ForegroundColor Green
