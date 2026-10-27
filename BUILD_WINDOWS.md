# Building Magnite on Windows

This project was built for Linux, but has been prepared for Windows compatibility. Follow these steps to build and run on Windows.

## Prerequisites

1. **Python 3.10+** - Download from [python.org](https://www.python.org/downloads/)
2. **Visual Studio 2022** with "Desktop development with C++" workload
3. **CMake 3.20+** - [cmake.org](https://cmake.org/download/)
4. **Qt 6** - Install via [Qt Online Installer](https://www.qt.io/download-qt-installer) (need Widgets, Network, and Svg modules)
5. **OpenSSL** - Can be installed via vcpkg or [slproweb.com](https://slproweb.com/products/Win32OpenSSL.html)

## Setup with vcpkg (Recommended)

```cmd
# Install vcpkg
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

# Install dependencies
C:\vcpkg\vcpkg install qtbase[widgets,network]:x64-windows qtsvg:x64-windows openssl:x64-windows
```

## Build

### Using the build script

```cmd
build_windows.bat
```

Or with PowerShell:
```powershell
.\build_windows.ps1
```

### Manual build

```cmd
# Clone and checkout dev branch
git clone https://github.com/Ronit9320/magnite_llm.git
cd magnite_llm
git checkout dev

# Setup Python environment
python -m venv .venv
.venv\Scripts\activate
pip install -e "backend[ml]"

# Build C++ app
cmake -S app -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

## Running

```cmd
build\Release\magnite-app.exe
```

Or from the build directory:
```cmd
cd build\Release
magnite-app.exe
```

The app will automatically look for the backend in `..\..\backend\src` and use the venv Python from `..\..\.venv\Scripts\python.exe`. Backend data is stored in `%APPDATA%\Magnite\` on Windows (or `~\.magnite` if using WSL paths, but on native Windows it's `%APPDATA%\Magnite`).
