@rem Copyright (c) 2026 Mizushi and local contributors
@rem SPDX-License-Identifier: CPAL-1.0
@echo off
setlocal
set "WSX_BOOTSTRAP=%~dp0bin\wsx_bootstrap.exe"
if not exist "%WSX_BOOTSTRAP%" goto :first_run
if not exist "%~dp0bin\wsx_boot_graphics.dll" goto :first_run
if not exist "%~dp0bin\wsx_sample_game.dll" goto :first_run

:launch
if "%~1"=="" (
    call "%~dp0Start-SignalRun.cmd"
    exit /b
)
"%WSX_BOOTSTRAP%" %*
exit /b %ERRORLEVEL%

:first_run
call :install
if errorlevel 1 exit /b %ERRORLEVEL%
goto :launch

:install
setlocal
if not exist "%~dp0native\CMakeLists.txt" (
    echo MizuBox source files are missing from "%~dp0native".
    exit /b 1
)

call "%~dp0tools\setup-build-env.cmd"
if errorlevel 1 exit /b 1

set "MIZUBOX_BUILD=%~dp0build-launcher"
echo [1/3] Configuring MizuBox...
cmake.exe -S "%~dp0native" -B "%MIZUBOX_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_TRY_COMPILE_CONFIGURATION=Release
if errorlevel 1 goto :install_failed

echo [2/3] Building MizuBox...
cmake.exe --build "%MIZUBOX_BUILD%" --parallel 4
if errorlevel 1 goto :install_failed

echo [3/3] Installing MizuBox...
cmake.exe --install "%MIZUBOX_BUILD%" --prefix "%~dp0." --component Runtime
if errorlevel 1 goto :install_failed
exit /b 0

:install_failed
echo MizuBox setup failed. See the build output above.
exit /b 1
