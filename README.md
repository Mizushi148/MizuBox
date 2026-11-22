# MizuBox

MizuBox by **Mizushi** is an experimental Xbox Series X|S compatibility layer for Windows x64. It implements parts of the Xbox runtime in C++20 and translates graphics work to Direct3D 12, including Shader Model 6.6 and DXR where the adapter supports them.

The included Signal Run sample exercises the native runtime. The test suite covers individual APIs and graphics paths. Commercial game compatibility remains experimental.

## Build and run

Open `Start-MizuBox.cmd` from File Explorer or run `.\Start-MizuBox.cmd` in PowerShell. On first run it finds the installed Visual Studio toolchain, builds and installs the Release runtime, and opens Signal Run. Later runs use the installed binaries.

Install Visual Studio or Build Tools with the Desktop development with C++ workload, a recent Windows SDK, CMake and Ninja. The launcher can use the CMake and Ninja bundled with Visual Studio.

The launcher accepts bootstrap arguments. Use `.\Start-MizuBox.cmd --sample --warp` to run Signal Run with WARP or `.\Run-GpuCheck.ps1` to inspect hardware support.

Run `.\Test-MizuBox.cmd` from PowerShell to build and run the full CTest and Python suites. The test runner uses `build-tests/` and requires Python 3.10 or newer for its Python checks.

This repository contains source, local diagnostics and test fixtures. Game executables, console system images and game assets are supplied separately by the user for optional integration checks.

See the [Windows build guide](docs/BUILD.md), [compatibility notes](docs/COMPATIBILITY.md), and [shader architecture](docs/SHADER_ARCHITECTURE.md).

## License and upstream

MizuBox is a fork of **AnyXboxSeries by edupode**. The project is distributed under the [Common Public Attribution License 1.0](LICENSE). See [CREDITS.md](CREDITS.md) for attribution and the [upstream repository](https://github.com/edupode/anyxboxseries).
