# Opening Magnite on macOS (testing build)

The macOS testing build is **not notarized** by Apple. macOS Gatekeeper therefore blocks the first launch of a copy you downloaded.
This is expected. You approve the app once, in System Settings, and macOS remembers the choice.

Only install a copy you got from the Magnite maintainers, and compare its checksum first:

```sh
shasum -a 256 ~/Downloads/Magnite-0.1.0-test.1-arm64.dmg
```

The result must match the published SHA-256.

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
standard account on macOS 27.0.1. Other versions have not been tested.

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
your Mac. The app runs with your user's permissions, keeps its data in `~/.magnite`, and talks to Hugging Face only for model and
embedding downloads (see [storage and privacy](STORAGE_AND_PRIVACY.md)).
