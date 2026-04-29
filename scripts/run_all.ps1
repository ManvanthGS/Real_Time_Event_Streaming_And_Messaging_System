# filepath: scripts/run_client_server.ps1
# Script to run client.exe and server.exe in separate terminals on Windows

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "..\build\ninja-debug"

$ServerExe = Join-Path $BuildDir "server.exe"
$ClientExe = Join-Path $BuildDir "client.exe"

# Check if executables exist
if (-not (Test-Path $ServerExe)) {
    Write-Error "Server executable not found at $ServerExe. Please build the project first."
    exit 1
}

if (-not (Test-Path $ClientExe)) {
    Write-Error "Client executable not found at $ClientExe. Please build the project first."
    exit 1
}

Write-Host "Starting Server and Client in separate terminals..."

# Start server in a new terminal
Start-Process powershell -ArgumentList "-NoExit", "-Command", "Write-Host 'Server running...'; & '$ServerExe'; Read-Host 'Press Enter to exit'" -Verb RunAs -WindowStyle Normal

# Wait a moment for server to start
Start-Sleep -Seconds 1

# Start client in a new terminal
Start-Process powershell -ArgumentList "-NoExit", "-Command", "Write-Host 'Client running...'; & '$ClientExe'; Read-Host 'Press Enter to exit'" -Verb RunAs -WindowStyle Normal

Write-Host "Both terminals should now be open."
