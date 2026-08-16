# Script to configure and build the project for Windows

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Join-Path $ScriptDir ".."

Set-Location $ProjectRoot

Write-Host "Configuring the project (windows-debug preset)..." -ForegroundColor Cyan
cmake --preset windows-debug

if ($LASTEXITCODE -ne 0) {
    Write-Error "Configuration failed!"
    exit 1
}

Write-Host "Building the project..." -ForegroundColor Cyan
cmake --build --preset windows-debug

if ($LASTEXITCODE -eq 0) {
    Write-Host "Build successful! Binaries are in build/windows-debug/" -ForegroundColor Green
} else {
    Write-Error "Build failed!"
    exit 1
}
