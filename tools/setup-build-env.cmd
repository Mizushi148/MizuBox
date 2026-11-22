@rem Copyright (c) 2026 Mizushi and local contributors
@rem SPDX-License-Identifier: CPAL-1.0
@echo off
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

exit /b 0

:missing_tools
echo MizuBox needs Visual Studio or Build Tools with Desktop development with C++, a recent Windows SDK, CMake and Ninja.
exit /b 1
