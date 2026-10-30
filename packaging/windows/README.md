# Windows native testing packages

The integration pipeline builds **x86-64 CPU** packages on `windows-2022`, with
MSVC 2022/Ninja, official Qt 6.8.3 `win64_msvc2022_64`, pinned vcpkg OpenSSL,
Python 3.12.12, uv 0.13.5 and Inno Setup 7.1.1. Native desktop acceptance on
Windows 10/11 remains separate from the hosted Windows Server environment.

The platform branch had source-build scripts but no installer or updater.
This implementation chooses a **per-user Inno Setup installer**: it supports
explicit install/upgrade/uninstall and Restart Manager file-lock handling,
without an administrator account or a resident service. The same stable AppId
is used across versions. No installed-app updater or Authenticode signing is
implied; those are Phase 5 work. See
[Inno Setup parameters](https://jrsoftware.org/ishelp/topic_setupcmdline.htm).

From an x64 VS developer shell, install pinned tools and dependencies as in
[the reusable workflow](../../.github/workflows/build-windows.yml), then run:

```powershell
$env:MAGNITE_REQUIRE_CLEAN = '1'
.\packaging\windows\build_package.ps1 -QtRoot C:/Qt/6.8.3/msvc2022_64 -VcpkgRoot C:/vcpkg -InnoCompiler C:/Inno/ISCC.exe
```

`tools.lock.json` pins Qt, vcpkg source and the checksum-verified Inno bootstrap.
`requirements-windows-x86_64.txt` and `constraints-windows-x86_64.txt` are
Windows-only locks, resolved using:

```sh
uv pip compile packaging/windows/requirements-windows-x86_64.txt backend/pyproject.toml --python-version 3.12 --python-platform x86_64-pc-windows-msvc --torch-backend cpu -o packaging/windows/constraints-windows-x86_64.txt
```

The builder stages into `build/packaging/windows/stage`, deploys Qt with
`windeployqt`, explicitly adds OpenSSL DLLs, includes MSVC runtime deployment,
and stages isolated Python plus a newly built backend wheel. It never copies a
developer virtualenv. License texts and a conservative superset of Qt
qtbase/qtsvg attribution notices are included; legal release review stays open.
The installation root is `%LOCALAPPDATA%/Programs/Magnite`; user data stays in
`%APPDATA%/Magnite` or `MAGNITE_HOME`.

All display labels derive from `_version.py`. Native numeric fields map the
canonical major/minor/patch (missing patch becomes zero) plus UPDATE_SEQUENCE:
`v0.1-preview`, sequence 1 → `0.1.0.1`. Each field is limited to 65535.
The executable version resource and installer numeric version use this mapping.
Stable identity parsing remains Phase 4.

`smoke_package.py` runs under the staged Python runtime and installs into a
disposable path with spaces. It exercises theme/UI/startup with poisoned host
Python, reinstallation/upgrade and external-data preservation, force-killed GUI
backend cleanup, restart, and uninstall. Use it on disposable runners: Qt
preferences use the current runner's user account. Real cross-version upgrades,
busy-file cancellation/recovery, signing and updater transactions need later
native acceptance; reinstallation of the candidate is not evidence for all of
those scenarios.

Backend lifetime uses a Windows Job Object with kill-on-close; normal Windows
shutdown uses the authenticated desktop-only shutdown endpoint so console
Python drains gracefully. A CRT byte-range lock enforces one backend per data
directory and is released by the OS on process death. Source discovery only
looks for an actual backend source directory, and installed runtimes are
selected before any developer Python probes.
