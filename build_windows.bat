@echo off
REM Build script for Windows
setlocal

REM Create venv if it doesn't exist
if not exist ".venv" (
    python -m venv .venv
)

REM Activate venv and install backend
call .venv\Scripts\activate.bat
pip install -e "backend[ml]"

REM Build with CMake (requires Qt6 and OpenSSL installed)
if not exist "build" mkdir build
cmake -S app -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

echo Build complete. Run build\Release\magnite-app.exe
