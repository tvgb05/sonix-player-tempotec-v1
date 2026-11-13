# Script build firmware TempoTec V1 bang Docker Desktop tren Windows
$ErrorActionPreference = "Stop"

Set-Location -Path $PSScriptRoot

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host " TempoTec V1 Local Build via Docker" -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan

# Kiem tra Docker Daemon
try {
    docker info | Out-Null
} catch {
    Write-Host "[!] Docker Desktop chua bat hoac chua chay! Hay mo Docker Desktop truoc khi chay script nay." -ForegroundColor Red
    exit 1
}

Write-Host "[1/2] Building/Checking Docker builder image..." -ForegroundColor Yellow
docker build -t sonix-v1-builder -f Dockerfile.builder .

Write-Host "[2/2] Running build inside Docker container..." -ForegroundColor Yellow
docker run --rm -v "${PSScriptRoot}:/workspace" sonix-v1-builder

Write-Host "Done!" -ForegroundColor Green
