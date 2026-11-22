# MizuBox

MizuBox by **Mizushi** is an experimental Xbox Series X|S compatibility layer for Windows x64. It implements parts of the Xbox runtime in C++20 and translates graphics work to Direct3D 12, including Shader Model 6.6 and DXR where the adapter supports them.

The included Signal Run sample exercises the native runtime. The test suite covers individual APIs and graphics paths. Commercial game compatibility remains experimental.

## Build and run

From an x64 Visual Studio developer shell with MSVC, the Windows SDK, CMake and Ninja installed:

~~~powershell
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix .
.\Start-MizuBox.cmd
~~~

The launcher opens Signal Run by default and accepts bootstrap arguments. Use `Start-MizuBox.cmd --sample --warp` to run the sample with WARP or `Run-GpuCheck.ps1` to inspect hardware support.

This repository contains source, local diagnostics and test fixtures. Game executables, console system images and game assets are supplied separately by the user for optional integration checks.

See the [Windows build guide](docs/BUILD.md), [compatibility notes](docs/COMPATIBILITY.md), and [shader architecture](docs/SHADER_ARCHITECTURE.md).

## License and upstream

MizuBox is a fork of **AnyXboxSeries by edupode**. The project is distributed under the [Common Public Attribution License 1.0](LICENSE). See [CREDITS.md](CREDITS.md) for attribution and the [upstream repository](https://github.com/edupode/anyxboxseries).
