# Building

The build uses CMake (3.21+) and vcpkg for SDL2, SDL2_mixer, nlohmann-json and GoogleTest.
**The game code is 32-bit only for now**: only the 32-bit Windows build runs the game. CMake refuses a
64-bit configure unless `-DREMC1_EXPERIMENTAL_64BIT=ON` is passed.

## Windows (supported)

Requirements: Visual Studio 2022 with the *Desktop development with C++* workload (it ships CMake and
vcpkg), Python 3 for the tools, and Git Bash for the helper script.

```bash
./build.sh                    # Debug   -> build/win-x86/Debug/remc1.exe
./build.sh win-x86-release    # Release -> build/win-x86/RelWithDebInfo/remc1.exe
```
`build.sh` uses the CMake and vcpkg bundled with Visual Studio. If Visual Studio is not installed at
the default location, set `VS_ROOT` (e.g. `VS_ROOT="/c/Program Files/Microsoft Visual Studio/2022/Community"`).

Without Git Bash, from a *Developer PowerShell for VS 2022*:
```powershell
$env:VCPKG_ROOT = "$env:VSINSTALLDIR\VC\vcpkg"
cmake --preset win-x86
cmake --build --preset win-x86-release
```
Opening the folder in Visual Studio also works (it reads `CMakePresets.json`). The SDL2 DLLs and the
font are copied next to the executable.

Other presets: `win-x86-asan` (AddressSanitizer build, used by `tools/asan_run.sh`) and
`win-x64-experimental` (64-bit; compiles, but the game does not run yet).

### Tests
```bash
REMC1_DATA_DIR=<datadir> ctest --preset win-x86-debug
```
Without `REMC1_DATA_DIR` the tests that need the game data (data hashes, replays) are skipped. Use
the CTest bundled with Visual Studio if the one on `PATH` is too old.

## Linux (not supported yet)

The game needs a 64-bit conversion before it can run on Linux (see the README). The build files
already contain an experimental preset that compiles the 64-bit build with system packages:
```bash
sudo apt install build-essential cmake ninja-build libsdl2-dev libsdl2-mixer-dev \
    nlohmann-json3-dev libgtest-dev
cmake --preset linux-debug
cmake --build --preset linux-debug
```
Expect compile errors and a binary that does not run until the 64-bit work is done.

## macOS (not supported yet)

Same situation as Linux. The experimental preset uses vcpkg:
```bash
brew install cmake ninja pkg-config
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
cmake --preset macos-debug
cmake --build --preset macos-debug
```
