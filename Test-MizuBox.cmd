@rem Copyright (c) 2026 Mizushi and local contributors
@rem SPDX-License-Identifier: CPAL-1.0
@echo off
setlocal
set "MIZUBOX_TEST_BUILD=%~dp0build-tests"

call :run_native
set "MIZUBOX_NATIVE_RESULT=%ERRORLEVEL%"
call :run_python
if errorlevel 1 exit /b 1
if not "%MIZUBOX_NATIVE_RESULT%"=="0" exit /b %MIZUBOX_NATIVE_RESULT%
echo MizuBox tests passed.
exit /b 0

:run_native
call "%~dp0tools\setup-build-env.cmd"
if errorlevel 1 exit /b 1
where /q ctest.exe
if errorlevel 1 (
    echo CTest was not found. Install the CMake command-line tools.
    exit /b 1
)

echo [1/4] Configuring the MizuBox test build...
cmake.exe -S "%~dp0native" -B "%MIZUBOX_TEST_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_TRY_COMPILE_CONFIGURATION=Release
if errorlevel 1 exit /b %ERRORLEVEL%

echo [2/4] Building MizuBox and its tests...
cmake.exe --build "%MIZUBOX_TEST_BUILD%" --parallel 4
if errorlevel 1 exit /b %ERRORLEVEL%

echo [3/4] Running the full CTest suite...
ctest.exe --test-dir "%MIZUBOX_TEST_BUILD%" --output-on-failure --no-tests=error
exit /b %ERRORLEVEL%

:run_python
echo [4/4] Running the Python tests...
set "PYTHONDONTWRITEBYTECODE=1"
py.exe -3 -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1
if not errorlevel 1 goto :python_launcher
python.exe -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1
if errorlevel 1 (
    echo Python 3.10 or newer is required to run the Python tests.
    exit /b 1
)
python.exe -m unittest discover -s "%~dp0tools" -v
exit /b %ERRORLEVEL%

:python_launcher
py.exe -3 -m unittest discover -s "%~dp0tools" -v
exit /b %ERRORLEVEL%
