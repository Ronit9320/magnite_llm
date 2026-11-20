# Installing Magnite Space on Windows (preview build)

The Windows preview installer is **not code-signed**. Windows and your browser
therefore warn you before it runs. This is expected for this preview; a signed
build is planned for a later release. You approve the installer once. Updates
installed from inside the app don't show these warnings again.

Only install a copy downloaded from the
[project's GitHub releases](https://github.com/Ronit9320/magnite_llm/releases),
and compare its checksum with the release's `SHA256SUMS` first. In PowerShell:

```powershell
Get-FileHash "$HOME\Downloads\Magnite-v0.2-preview-windows-x86_64-setup.exe" -Algorithm SHA256
```

The hash must match the line for that file in `SHA256SUMS` (letter case doesn't matter).

## Steps

1. **Browser warning.** Edge or Chrome may say the file "isn't commonly downloaded".
   Choose **Keep** (in Edge, open **…**, then **Keep**, then **Show more → Keep anyway**).
2. **Run the installer.** Windows shows "Windows protected your PC". Click
   **More info**, check that the app name is the setup file you downloaded, then
   click **Run anyway**.
3. The installer needs no administrator rights. It installs for your user under
   `%LOCALAPPDATA%\Programs\Magnite` and adds a Start menu entry. Your data lives
   separately in `%APPDATA%\Magnite`.

The exact wording differs between Windows versions and browsers.

## Smart App Control

On Windows 11 PCs where **Smart App Control** is on (Windows Security → App &
browser control), Windows blocks unsigned apps without a **Run anyway** option.
Turning Smart App Control off is a system-wide change that Windows does not let
you simply turn back on, so we don't recommend it for this preview. Use another
PC, or wait for a signed release.

## Updates

Settings → Updates checks for newer previews. A download is verified against a
signature built into the app and its SHA-256 before anything runs. When you choose
**Restart to install**, Magnite Space closes, the new installer runs silently over
your installation and the new version starts. Your models, datasets, adapters and
chats stay where they are.

If the new version installs but does not start, Windows keeps it installed
(there is no automatic rollback on Windows). Settings → Updates shows what
happened the next time you open the app; to go back, run the previous version's
installer from its release page. Updates need the copy installed by the installer;
a source build or a copied folder can check for updates but not install them.

## If it still won't install or start

- **"This app can't run on your PC".** The preview needs 64-bit Windows 10 version
  1809 or later, or Windows 11, on an x64 processor.
- **Antivirus quarantines a file.** Some antivirus tools flag new, unsigned
  programs. Restore the file only if its checksum matches, then report the
  detection on GitHub so we can submit it for review.
- **Nothing happens or it quits at once.** Make sure no other Magnite Space is
  already running on the same data folder, then see
  [troubleshooting](TROUBLESHOOTING.md) and the
  [Windows testing notes](../packaging/windows/TESTING.md).

## What approving means

Run anyway tells Windows to run this one installer despite the missing publisher
signature. It doesn't change any other protection on your PC. The app runs with
your user's permissions. Network activity can include Hugging Face searches and
downloads, GitHub update checks and a configured Ollama service. See
[storage and privacy](STORAGE_AND_PRIVACY.md).
