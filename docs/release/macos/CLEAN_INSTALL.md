# Clean-install check — second macOS account (Phase 10)

This checks that the Magnite testing build installs and runs as a **downloaded** app for a user who has never had a developer setup.
You do it from a **second macOS user account** on the same Mac. That is a *proxy* for a clean machine, not a substitute for one.

It takes about 20 minutes. Write everything you see into `RESULTS.md` in the kit folder, **word for word** where it says so.

## Before you start (in the developer account)

1. Build the kit: `bash packaging/macos/stage_clean_install.sh`. It fills `/Users/Shared/magnite-clean-install/` with:
   - `Magnite-download.dmg`: a copy of `dist/Magnite-0.1.0-test.1-arm64.dmg` marked as downloaded by Safari (quarantined);
   - `smoke_bundle.sh`: the automated run check;
   - `CLEAN_INSTALL.md`: this file;
   - `RESULTS.md`: the form you fill in.
2. Make sure `/Applications/Magnite.app` does **not** exist. `/Applications` is shared by every account on the Mac.
3. Create the second account if there is none: **System Settings → Users & Groups → Add User…**. A *Standard* account is the realistic
   choice; you will need an administrator's name and password once, to install into `/Applications`.

## In the second account

Log in to the second account (Fast User Switching is fine). Do not install anything else first.

### 1. Record the environment

Open **Terminal** and run the following, then paste the output into `RESULTS.md` § 1:

```sh
sw_vers; whoami; echo "PATH=$PATH"
for t in python3 brew uv; do printf '%s: ' "$t"; command -v "$t" || echo "not found"; done
ls -d ~/.magnite ~/.cache/huggingface /opt/homebrew 2>&1
```

`/usr/bin/python3` may exist; it is a system stub, and the app must not use it.

### 2. "Download" the image

```sh
cp /Users/Shared/magnite-clean-install/Magnite-download.dmg ~/Downloads/
xattr -p com.apple.quarantine ~/Downloads/Magnite-download.dmg
```

The second command must print a quarantine value such as `0181;6adb865e;;`. `cp` keeps the flag but may drop the `Safari` name; that
is fine. Paste it into § 2. If it prints "No such xattr", stop and note it: the test would not be a real download.

### 3. Open the image and install

1. In Finder, open **Downloads** and double-click `Magnite-download.dmg`. If any dialog appears, copy its text into § 3 **word for word**.
   A screenshot helps: ⇧⌘4, then Space, then click the dialog. Save it into `/Users/Shared/magnite-clean-install/`.
2. In the window that opens, drag **Magnite** onto **Applications**. If macOS asks for an administrator password, enter it and note that it asked.
3. Eject the image (the ⏏ button next to it in Finder's sidebar).

### 4. First launch: Gatekeeper

1. Open **Applications** and double-click **Magnite**.
2. macOS should refuse to open it, because this testing build is not notarized. Copy the dialog **title, message and buttons word for word** into
   § 4 and take a screenshot. Click **Done** (or **OK**/**Cancel**, whatever it offers). Do **not** choose "Move to Trash".
3. Open **System Settings → Privacy & Security**, scroll to **Security**. There should be a line like "Magnite was blocked to protect your
   Mac." with an **Open Anyway** button. Copy the line, click **Open Anyway**, authenticate if asked, then copy and confirm the next dialog
   (usually another "Open Anyway"). Record every dialog word for word in § 4.

### 5. First run

1. Note the time when the window appears. Watch the status in the top-right corner. Record how long it takes to change from
   "Connecting" to connected, in § 5.
2. While the app is open, run this in Terminal and paste the output into § 5. It shows where macOS actually runs the app from
   (an `AppTranslocation` path would be a finding):

   ```sh
   ps -axo pid,command | grep -E "MacOS/Magnite|backend.main" | grep -v grep
   ```

### 6. One real chat

1. In **Library**, search for `Segilmez06/SmolLM2-135M-Instruct-Q4_K_M-GGUF`, select it and click **Download**. Wait for it to finish (≈ 100 MB).
2. Go to **Playground**, choose that model, and send: `What are the three primary colours?`
3. Record in § 6 how long it took until the first words appeared. **The first GPU use for a new user is expected to pause ~10 s** while
   macOS prepares GPU shaders. Copy the first sentence of the answer.
4. Send a second message (`Name one more colour.`) and record its time to first words. It should be much faster.

### 7. Automated check

Quit Magnite (**⌘Q**), then run the following and paste the whole output into § 7:

```sh
bash /Users/Shared/magnite-clean-install/smoke_bundle.sh /Applications/Magnite.app ~/magnite-smoke
pgrep -fl backend.main || echo "nothing running"
```

### 8. Clean up

```sh
rm -rf ~/.magnite ~/magnite-smoke ~/Downloads/Magnite-download.dmg
```

Then drag **Magnite** from **Applications** to the Trash (it is shared with the other account) and empty the Trash. Switch back to the
developer account and say "Phase 10 results are in /Users/Shared/magnite-clean-install/RESULTS.md". The second account can be deleted
afterwards (System Settings → Users & Groups).
