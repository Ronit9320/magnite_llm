# Magnite Space v0.2-preview — Windows x86-64

This is magnite-spacev0.2, an unfinished development preview for Windows 10 1809
or newer and Windows 11 x64. Blueprints and Tool mode are not included in this
release. The installer is not Authenticode-signed, so Windows SmartScreen warns
before it runs; see the [first-launch guide](../../docs/WINDOWS_FIRST_LAUNCH.md).
Hosted checks use Windows Server 2022; desktop Windows coverage is recorded
separately.

Run `Magnite-v0.2-preview-windows-x86_64-setup.exe`. It installs per user under
`%LOCALAPPDATA%\Programs\Magnite`, without administrator rights. Qt, OpenSSL,
the MSVC runtime and an isolated Python CPU runtime are bundled. Host Python is
not needed. Models are downloaded separately.

Settings → Updates checks for newer previews. The app verifies the signed update
metadata and the installer's SHA-256, then on **Restart to install** closes and
runs the new installer silently over this installation and starts the new
version. Inno Setup undoes an installation that fails; a version that installs
but doesn't start is reported in Settings → Updates, not rolled back. To go back,
run the previous version's installer.

To upgrade by hand, quit Magnite Space first. The installer uses Windows Restart
Manager to detect locked files and does not forcibly close applications.

Data stays under `%APPDATA%\Magnite` (or `MAGNITE_HOME`), outside installation.
Uninstalling removes the application, not models, datasets, adapters or settings.
Use a disposable `MAGNITE_HOME` and Hugging Face cache for acceptance testing.
Do not delete personal data to test a clean install.
