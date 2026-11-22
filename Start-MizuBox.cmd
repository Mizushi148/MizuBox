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

if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :find_tools
set "MIZUBOX_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%MIZUBOX_VSWHERE%" goto :missing_tools
set "MIZUBOX_VS="
for /f "usebackq delims=" %%I in (`"%MIZUBOX_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "MIZUBOX_VS=%%I"
if not defined MIZUBOX_VS goto :missing_tools

call "%MIZUBOX_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo
if errorlevel 1 goto :missing_tools

:find_tools
if defined VSINSTALLDIR set "PATH=%VSINSTALLDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSINSTALLDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where /q cl.exe
if errorlevel 1 goto :missing_tools
where /q cmake.exe
if errorlevel 1 goto :missing_tools
where /q ninja.exe
if errorlevel 1 goto :missing_tools

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

:missing_tools
echo MizuBox needs Visual Studio or Build Tools with Desktop development with C++, a recent Windows SDK, CMake and Ninja.
exit /b 1

:install_failed
echo MizuBox setup failed. See the build output above.
exit /b 1
