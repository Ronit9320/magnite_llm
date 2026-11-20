# Opening Magnite Space on macOS (preview build)

The macOS preview build is **not notarized** by Apple. macOS Gatekeeper therefore blocks the first launch of a copy you downloaded.
This is expected. You approve the app once, in System Settings, and macOS remembers the choice. Updates installed from inside
the app don't ask again.

Only install a copy downloaded from the [project's GitHub releases](https://github.com/Ronit9320/magnite_llm/releases), and
compare its checksum with the release's `SHA256SUMS` first:

```sh
shasum -a 256 ~/Downloads/Magnite-v0.2-preview-arm64.dmg
```

The result must match the line for that file in `SHA256SUMS`.

## Steps

1. Open the `.dmg` and drag **Magnite** onto **Applications**. Eject the image.
2. Open **Magnite** from Applications (double-click). macOS refuses to open it.
   Dismiss the dialog without moving the app to the Trash.
3. Open **System Settings → Privacy & Security** and scroll to the **Security** section. A message about Magnite being blocked appears
   there, with an **Open Anyway** button.
4. Click **Open Anyway**, authenticate with your password or Touch ID, and confirm in the next dialog.
5. Magnite starts. Later launches work normally; you don't repeat these steps for the same copy.

The Open Anyway button only appears for a while after a blocked launch. If it is gone, open the app again to bring it back.

On a standard (non-administrator) account, copying the app into `/Applications` needs an administrator's name and password.

The exact wording of the dialogs differs between macOS versions and has not been recorded for this build. This flow was completed on a
standard account on macOS 27.0.1. That physical-account flow has not been recorded on other versions; later hosted
macOS 15 bundle checks are separate evidence, not a native Gatekeeper walkthrough.

## Updates

Settings → Updates checks for newer previews. A download is verified against a signature built into the app and its SHA-256
before anything changes. When you choose **Restart to install**, Magnite Space closes, swaps in the new version and starts it.
If the new version doesn't start, the previous one is put back automatically. Your models, datasets, adapters and chats stay in
`~/.magnite`.

- Keep the app in **Applications** (or another folder you can write to) and open it from there. A copy opened straight from the
  disk image, or from Downloads before you moved it, runs from a read-only location and can't update itself; the app tells you
  to move it.
- On a standard (non-administrator) account, `/Applications` isn't writable, so the app can't replace itself there. Use
  `~/Applications` instead, or ask an administrator to install updates.
- macOS may ask whether Magnite Space can **update or delete other applications** (System Settings → Privacy & Security →
  App Management). Allow it for updates to install. If you decline, the update stops and the current version keeps running.
- The previous version is kept until the next update, in a hidden `.magnite-update-…` folder beside the app.

## If it still won't open

- **"Damaged" or "can't be opened" even after Open Anyway.** Check the checksum above. A truncated or modified download won't open.
- **Remove the download flag (Terminal).** This is an alternative to the steps above and was **not tested** on this build. It tells macOS
  you trust the file, so use it only on a copy whose checksum matches:

  ```sh
  xattr -dr com.apple.quarantine /Applications/Magnite.app
  ```

- **Apple silicon only.** Intel Macs cannot run this build. It needs macOS 14 or newer.
- **Nothing happens or it quits at once.** Make sure no other Magnite is already running on the same data folder, then see
  [troubleshooting](TROUBLESHOOTING.md) and [the testing notes](../packaging/macos/TESTING.md).

## What approving means

Approving the app tells macOS to run this one copy despite the missing Apple notarization. It doesn't change any other protection on
your Mac. The app runs with your user's permissions and keeps its data in
`~/.magnite` unless overridden. Network activity can include Hugging Face searches,
metadata and weight downloads, GitHub update checks, and a configured Ollama
service. See [storage and privacy](STORAGE_AND_PRIVACY.md).
