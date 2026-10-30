# Build script for Windows PowerShell (VS 2026 + vcpkg-provided Qt6/OpenSSL)

# Use the Python 3.12 venv (3.15 has no wheels for most ML packages)
$venv = ".venv312"
if (-not (Test-Path "$venv\Scripts\pip.exe")) {
    py -3.12 -m venv $venv
}

# Activate venv and install backend
& ".\$venv\Scripts\Activate.ps1"
pip install -e "backend[ml]"

# Build with CMake (Qt6 and OpenSSL come from vcpkg)
$env:VCPKG_ROOT = "C:\vcpkg"
cmake -S app -B build -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release

Write-Host "Build complete. Run build\Release\magnite-app.exe"
