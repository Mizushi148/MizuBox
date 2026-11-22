# Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
# SPDX-License-Identifier: CPAL-1.0
#
# 1-Click MizuBox (by Mizushi — Fork of AnyXboxSeries) Setup, Package Check, GPU Probe & Offline Shader/PSO Pre-Warmer
# Usage:
#   .\wsx_setup.ps1 [-PackageDir ".."] [-PrewarmMax 32] [-SkipGpuCheck]

param(
    [string]$PackageDir = "",
    [int]$PrewarmMax = 29,
    [switch]$SkipGpuCheck
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($PackageDir -eq "") { $PackageDir = Split-Path -Parent $Root }

$PkgTool = @( (Join-Path $Root "bin\wsx_package_tool.exe"), (Join-Path $Root "build\wsx_package_tool.exe"), (Join-Path $Root "native\build\wsx_package_tool.exe") ) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $PkgTool) {
    Write-Error "wsx_package_tool.exe not found. Build MizuBox first."
}

Write-Host "[1/3] Discovering Xbox Series X|S package in $PackageDir..." -ForegroundColor Green
& $PkgTool --find $PackageDir

Write-Host "`n[2/3] Generating CNG SHA-256 package integrity manifest..." -ForegroundColor Green
$Manifest = Join-Path $Root "wsx_package.sha256"
& $PkgTool --check $PackageDir --manifest $Manifest

$FsrDll = Join-Path $PackageDir "amd_fidelityfx_upscaler_xbox.dll"
if (Test-Path $FsrDll) {
    Write-Host "`n[3/3] Pre-warming RDNA2 -> SM 6.6 DXIL + Binary PSO Cache (resume enabled)..." -ForegroundColor Green
    $CacheDir = Join-Path $Root "wsx_shader_cache"
    & $PkgTool --prewarm-pso $FsrDll --max $PrewarmMax --cache-dir $CacheDir --resume
}

if (-not $SkipGpuCheck) {
    $GpuScript = Join-Path $Root "Run-GpuCheck.ps1"
    if (Test-Path $GpuScript) {
        Write-Host "`n[GPU] Running 408-Ray DXR Differential & Wave64 Hardware Check..." -ForegroundColor Cyan
        & $GpuScript
    }
}
