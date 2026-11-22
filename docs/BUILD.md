# Build on Windows

Install MSVC, CMake, Ninja and a recent Windows SDK with Direct3D 12 root signature 1.2 headers. The SDK's DirectX Shader Compiler provides `dxcompiler.dll` and `dxil.dll` for runtime shader translation.

For an automatic Release installation and launch, run `.\Start-MizuBox.cmd` from PowerShell or open it from File Explorer. The launcher finds Visual Studio, configures the build in `build-launcher/` and installs the runtime into `bin/`. Subsequent launches use the installed runtime.

For a manual build with tests, open the x64 Visual Studio developer shell and run these commands from the repository root:

~~~powershell
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix . --component Runtime
.\Start-MizuBox.cmd
~~~

Run the native sample, diagnostics or a compatible package after installing:

~~~powershell
.\Start-MizuBox.cmd --sample --warp
.\Start-MizuBox.cmd --build 'C:\Games\SampleBuild'
.\bin\wsx_graphics_probe.exe --warp
.\Run-GpuCheck.ps1
.\wsx_setup.ps1 -PackageDir 'C:\Games\SampleBuild'
~~~

The test suite uses the included sample and synthetic fixtures. Hardware tests for mesh shaders, Shader Model 6.6 and ray tracing require a capable D3D12 device. The Signal Run tests exercise the preferred adapter and WARP.

Optional integration checks take the paths to local inputs at configure time:

~~~powershell
cmake -S native -B build "-DMIZUBOX_TEST_SHADER_ARCHIVE=C:/Games/SampleBuild/shaders.rpf" "-DMIZUBOX_TEST_FSR_DLL=C:/Games/SampleBuild/amd_fidelityfx_upscaler_xbox.dll"
ctest --test-dir build --output-on-failure
~~~

The import inspector uses Python 3.10 or newer and DUMPBIN from the Visual Studio developer shell.

~~~powershell
python .\tools\inspect_xbox_imports.py 'C:\Games\SampleBuild' 'game.exe' --shims .\bin --output .\imports.txt
python -m unittest discover -s tools
~~~
