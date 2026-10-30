# Building Magnite on Windows

Source builds require Windows x64, Python 3.12, CMake 3.20+, Visual Studio with
Desktop C++ tools, and vcpkg Qt6 Widgets/Network/Svg plus OpenSSL:

```cmd
C:\vcpkg\vcpkg install qtbase[widgets,network]:x64-windows qtsvg:x64-windows openssl:x64-windows
build_windows.bat
```

The default generator is Visual Studio 18 2026. With VS 2022:

```powershell
.\build_windows.ps1 -Generator 'Visual Studio 17 2022' -VcpkgRoot C:/vcpkg
```

The script stops on failed commands, creates `.venv312`, installs the backend,
and builds `build/Release/magnite-app.exe`. The client discovers the checkout
from that multi-config directory, preferring `.venv312` then `.venv` on Windows.
Installed packages always use their isolated adjacent Python before any source
or host interpreter lookup. Data lives in `%APPDATA%/Magnite`, overridden by
`MAGNITE_HOME`. The desktop authenticates requests and supervises its backend
process tree; a byte-range lock prevents simultaneous writers to one data folder.

For the native CPU installer and artifact-only hosted workflow, use
[Windows packaging instructions](packaging/windows/README.md).
