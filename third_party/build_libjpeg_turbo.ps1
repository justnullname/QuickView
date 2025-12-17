#!/usr/bin/env pwsh
# Build libjpeg-turbo from source (submodule)
# Run this after: git submodule update --init --recursive

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceDir = Join-Path $scriptDir "libjpeg-turbo"
$buildDir = Join-Path $sourceDir "build"
$outputDir = Join-Path $scriptDir ".." "QuickView_VS2026_X64" "src" "QuickView2026"

Write-Host "=== Building libjpeg-turbo ===" -ForegroundColor Cyan

# Check if source exists
if (-not (Test-Path (Join-Path $sourceDir "CMakeLists.txt"))) {
    Write-Error "libjpeg-turbo source not found. Run: git submodule update --init --recursive"
    exit 1
}

# Create build directory
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

Push-Location $buildDir

try {
    # Configure
    Write-Host "Configuring..." -ForegroundColor Yellow
    cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF
    
    # Build
    Write-Host "Building..." -ForegroundColor Yellow
    cmake --build . --config Release --parallel
    
    # Copy outputs
    Write-Host "Copying outputs..." -ForegroundColor Yellow
    
    $includeDir = Join-Path $outputDir "include" "libjpeg-turbo"
    $libDir = Join-Path $outputDir "lib" "x64"
    
    # Create directories
    if (-not (Test-Path $includeDir)) {
        New-Item -ItemType Directory -Path $includeDir -Force | Out-Null
    }
    if (-not (Test-Path $libDir)) {
        New-Item -ItemType Directory -Path $libDir -Force | Out-Null
    }
    
    # Copy headers
    Copy-Item (Join-Path $sourceDir "turbojpeg.h") $includeDir -Force
    Copy-Item (Join-Path $buildDir "jconfig.h") $includeDir -Force
    
    # Copy library
    Copy-Item (Join-Path $buildDir "Release" "turbojpeg-static.lib") $libDir -Force
    
    Write-Host "=== Build complete! ===" -ForegroundColor Green
    Write-Host "Library: $libDir\turbojpeg-static.lib"
    Write-Host "Headers: $includeDir"
    
} finally {
    Pop-Location
}
