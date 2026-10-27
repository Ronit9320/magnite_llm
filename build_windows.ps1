# Build script for Windows PowerShell
param()

# Create venv if it doesn't exist
if (-not (Test-Path ".venv")) {
    python -m venv .venv
}

# Activate venv and install backend
& .\.venv\Scripts\Activate.ps1
pip install -e "backend[ml]"

# Build with CMake (requires Qt6 and OpenSSL installed)
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}
cmake -S app -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

Write-Host "Build complete. Run build\Release\magnite-app.exe"
