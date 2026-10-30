@echo off
REM Build script for Windows (VS 2026 + vcpkg-provided Qt6/OpenSSL)
setlocal

REM Use the Python 3.12 venv (3.15 has no wheels for most ML packages)
set "VENV=.venv312"
if not exist "%VENV%\Scripts\pip.exe" (
    py -3.12 -m venv %VENV%
)

REM Activate venv and install backend
call %VENV%\Scripts\activate.bat
pip install -e "backend[ml]"

REM Build with CMake (requires Qt6 and OpenSSL, installed via C:\vcpkg)
set "VCPKG_ROOT=C:\vcpkg"
if not exist "build" mkdir build
cmake -S app -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release

echo Build complete. Run build\Release\magnite-app.exe
endlocal
