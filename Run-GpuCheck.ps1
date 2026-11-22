# Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
# SPDX-License-Identifier: CPAL-1.0
#
# Portable zero-game-files GPU compatibility & 408-ray DXR differential BVH probe.
# Usage: .\Run-GpuCheck.ps1 [-ReportPath "my-gpu.md"] [-Warp]

param(
    [string]$ReportPath = "",
    [switch]$Warp
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Candidates = @(
    (Join-Path $Root "bin\wsx_gpu_check.exe"),
    (Join-Path $Root "build\wsx_gpu_check.exe"),
    (Join-Path $Root "native\build\wsx_gpu_check.exe")
)

$Exe = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Exe) {
    Write-Error "wsx_gpu_check.exe not found. Build and install MizuBox first. See docs/BUILD.md."
}

$ArgsList = @()
if ($Warp) { $ArgsList += "--warp" }
if ($ReportPath -ne "") {
    $ArgsList += "--report"
    $ArgsList += $ReportPath
} else {
    $ArgsList += "--report"
}

Write-Host "[MizuBox] Running GPU Compatibility & 408-Ray DXR Probe: $Exe" -ForegroundColor Green
& $Exe @ArgsList
exit $LASTEXITCODE
