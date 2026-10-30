# Windows x86-64 testing package

This is an unsigned development preview, built for Windows 10 1809 or newer
and Windows 11 x64. The hosted check uses Windows Server 2022; desktop Windows
device coverage must be recorded separately. No Authenticode signature or
installed-app automatic updater is supplied yet.

Run the `Magnite-<canonical version>-windows-x86_64-setup.exe` installer. It
installs per user under `%LOCALAPPDATA%\Programs\Magnite`, without administrator
rights. Qt, OpenSSL, the MSVC runtime and an isolated Python CPU runtime are
bundled. Host Python is not needed. Models are downloaded separately.

Quit Magnite before upgrading or uninstalling. The installer uses Windows
Restart Manager to detect locked files and does not forcibly close applications.
Automatic download, verified updater installation and recovery are Phase 5 work.

Data stays under `%APPDATA%\Magnite` (or `MAGNITE_HOME`), outside installation.
Uninstalling removes the application, not models, datasets, adapters or settings.
Use a disposable `MAGNITE_HOME` and Hugging Face cache for acceptance testing.
Do not delete personal data to test a clean install.
