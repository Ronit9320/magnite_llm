# Release pipeline and application updates

Releases are published from `release/**` branches by one manual workflow,
`.github/workflows/release.yml`. It builds and tests Linux, macOS and Windows
from one commit and publishes them together as a GitHub prerelease with signed
update metadata for each platform. Installed apps on all three platforms update
themselves from those releases. The current release branch is
`release/magnite-spacev0.2` (v0.2-preview).

## Branches and what runs

| Event | `verify.yml` + `native-packages.yml` | Publishes? |
| --- | --- | --- |
| Push to a feature branch | — | No |
| PR into `dev`, `master` or `release/**` | Build and test all three platforms (code/packaging/workflow changes) | No |
| Push to `dev` or `master` | Build and test all three platforms | No |
| Push to `release/**` | Build and test all three platforms | No |
| Manual **Publish preview release** on `release/**` | Builds, tests, signs and publishes | **Yes**, after the checks below |
| Push of a version tag on a `release/**` commit | Builds, tests, signs, creates a **draft** | Draft only; you publish it |
| Manual run on any other branch | `release.yml` refuses | No |

Documentation-only changes skip the automatic builds. `linux-preview.yml` is a
manual Linux-only package build and never publishes.

`dev` and `master` keep every feature that is in development. A release branch is
cut from `dev` and only receives fixes, by `git cherry-pick` from `dev`. Never
merge a release branch back: its version, release notes and feature switches
belong to that release only. Changes to the release machinery (workflows,
updater, packaging) are made on `dev`, or cherry-picked there from the release
branch, so the next release branch starts with them.

GitHub only shows the **Run workflow** button for workflows that exist on the
default branch (`master`). Merge `release.yml` into `dev` and promote it to
`master` before the first release from a branch; the run itself always uses the
release branch's copy of the workflow.

## Cutting a release

1. Branch from tested `dev`: `git switch -c release/<name> origin/dev`.
2. In `backend/src/backend/_version.py` set `RELEASE_VERSION` (format
   `vMAJOR.MINOR[.PATCH]-preview[.N]`), `RELEASE_NAME` (the GitHub release title)
   and an `UPDATE_SEQUENCE` higher than every published release. Change
   `DATA_SCHEMA` only if persistent formats changed; installed apps refuse
   updates with a different schema.
3. In `backend/src/backend/_features.py`, switch off features that are not ready.
4. Write `docs/release/RELEASE_NOTES.md`; it becomes the release description.
   Update the version-specific user docs (README, INSTALL, first-launch guides,
   packaging `TESTING.md` files, CHANGELOG).
5. Push the branch. The automatic runs must pass on all three platforms.
6. Start `release.yml` in one of two ways:
   - **Tag push (works without the workflow on `master`):** tag the tested commit
     of the release branch with the exact `RELEASE_VERSION` and push the tag, for
     example `git tag -a v0.2-preview -m magnite-spacev0.2 && git push origin
     v0.2-preview`. The run always stops at a draft; publish it on GitHub after
     checking it. The guard refuses a tag that is not on a `release/**` branch or
     does not match `RELEASE_VERSION`.
   - **By hand:** Actions → **Publish preview release** → choose the release
     branch and type the exact `RELEASE_VERSION` into `confirm_version`. Use
     `draft_only` to stop at a draft.
7. Install the published packages on real machines and check an update from the
   previous release (see the release checklist).

For a follow-up preview on the same branch (for example `v0.2-preview.1`), apply
the fixes, bump `RELEASE_VERSION` and `UPDATE_SEQUENCE`, update the notes, and run
the workflow again. A published version is never replaced.

## What `release.yml` checks and does

1. **Guard** (read-only): the ref is `release/**`; the checkout is clean;
   `confirm_version` equals `RELEASE_VERSION`; no release or tag with that name
   exists (an API error is not treated as "missing"); `UPDATE_SEQUENCE` is higher
   than the sequence in every published `update-*.json`
   (`packaging/check_release_sequence.py`).
2. **Build**: the reusable `build-linux.yml`, `build-macos.yml` and
   `build-windows.yml` build and test the exact guard commit, as in normal CI.
3. **Publish** (the only job with `contents: write` and the signing key, in the
   `release` environment): checks the complete artifact set against the commit
   (`verify_artifact_set.py`), signs `update-linux-x86_64.json`,
   `update-macos-arm64.json` and `update-windows-x86_64.json`
   (`release_metadata.py`), writes `SHA256SUMS`, creates a draft prerelease tagged
   at the tested commit, uploads every asset, checks the upload list, then
   publishes it as a prerelease that is not marked latest.

Add required reviewers to the `release` environment (Settings → Environments)
to make a second person approve each publication.

Release assets:

```text
Magnite-<version>-linux-x86_64.AppImage
Magnite-<version>-arm64.dmg                 first install on macOS
Magnite-<version>-macos-arm64.zip           macOS in-app updates (same signed bundle)
Magnite-<version>-windows-x86_64-setup.exe
update-{linux-x86_64,macos-arm64,windows-x86_64}.json and .json.sig
linux-compatibility.json, TESTING.md (Linux), SHA256SUMS
```

## Build profile: switching unfinished features off

`backend/src/backend/_features.py` holds `BLUEPRINTS_AND_TOOLS`. CMake generates
`build_features.h` from it, the backend imports it and packaging reads it through
`packaging/identity.py`. With it `False`:

- the client has no Blueprints workspace and no Tool/Blueprint conversation modes;
  the Settings profile hides their counts; Docs hides their articles and uses
  alternate wording in a few general ones (`requires` and
  `without_blueprints_and_tools` in the docs catalog);
- the backend mounts no `/blueprints` or `/inference/tool-runs` routes and never
  creates `blueprints.db`;
- packages leave out Node.js and Pi's npm dependencies; the Pi smoke check
  instead confirms they are absent.

The code still compiles and its unit tests still run, and `DATA_SCHEMA` does not
change, so switching a feature back on in a later release needs no migration.
Prefer this over deleting unfinished work on a release branch: a large deletion
would conflict with every fix cherry-picked from `dev`.

## Installed-app updates

All platforms share one flow in `app/src/update_manager.cpp`:

1. List up to 100 GitHub releases (prereleases included; `/latest` would skip
   them), sort the preview tags by version, and pick the newest one that has this
   platform's `update-<platform>.json` and signature.
2. Verify the Ed25519 signature against `packaging/keys/update-public.pem` built
   into the client, then the version, sequence (higher than the compiled one and
   the highest one this machine has started), `DATA_SCHEMA`, platform,
   architecture, exact download URL and the platform's minimum OS
   (`minimum_glibc`, `minimum_macos`, `minimum_windows`).
3. Download with a size bound, check the SHA-256 and the file type.
4. On **Restart to install**, ask the backend to quiesce, stop it gracefully,
   start the platform's installer step detached, wait for it to report `ready`,
   and quit.

| Platform | Install step | If the new version doesn't start |
| --- | --- | --- |
| Linux | `packaging/update_helper.cpp`: atomic `renameat2` exchange of the AppImage | Exchanged back and relaunched |
| macOS | `packaging/macos/update_install.sh`: the client unpacks the `.zip` beside the app first, the script renames the old bundle into the transaction folder and moves the new one in | Old bundle moved back and reopened |
| Windows | `packaging/windows/update_install.ps1`: waits for the client and backend to exit, runs the setup `.exe` with `/VERYSILENT` over the installation | Reported only; Inno Setup undoes failed installs, but not a successful install that won't start |

The new version confirms startup by writing its version to the health file
named by `--update-health-file` once its backend is ready. The transaction folder
(`.magnite-update-*`, beside the app on Linux/macOS, in `%TEMP%` on Windows) keeps
the logs and, on Linux and macOS, the previous copy. macOS and Windows remove the
previous successful transaction when the next update starts.

Where an app cannot replace itself (source builds, an AppImage that is not
writable, a macOS app opened from the disk image or App Translocation, a Windows
folder not made by the installer) it still checks and reports updates but offers
no install.

Tests: `magnite-update-tests` (manifest rules for all platforms),
`packaging/tests/test_update_helper.py` (Linux helper),
`packaging/tests/test_macos_update_install.py` (the macOS script with stand-in
`open`/`xattr`), and the Windows package smoke test, which runs the real update
script against the built installer. An update between two published releases on
physical Macs and PCs is still a manual release check.

## Code signing and first-launch warnings

| Platform | Today | Effect |
| --- | --- | --- |
| Linux | No OS signing needed | None |
| macOS | Ad-hoc signed, not notarized | Gatekeeper blocks the first launch; users approve it in System Settings → Privacy & Security ([guide](../MACOS_FIRST_LAUNCH.md)) |
| Windows | Not Authenticode-signed | SmartScreen shows "Windows protected your PC" (More info → Run anyway); Smart App Control blocks it outright ([guide](../WINDOWS_FIRST_LAUNCH.md)) |

In-app updates avoid these prompts: the client downloads without the quarantine
flag (macOS) or Mark of the Web (Windows) that browsers add, and every update is
checked against the project's own Ed25519 key. That key is the trust anchor for
updates; OS signing only affects first installs and OS reputation checks.

There is no free way to remove the first-launch warnings:

- **macOS** needs a Developer ID certificate from the Apple Developer Program
  (paid yearly membership), `codesign --options runtime` with that identity, and
  notarization with `notarytool` plus stapling. Add these to `build-macos.yml`
  as secrets once the account exists.
- **Windows** needs an Authenticode signature. Options include Microsoft's
  managed signing service on Azure (Trusted Signing: a low-cost monthly service
  with identity validation and eligibility rules), SignPath Foundation's free
  signing for qualifying open-source projects, or an OV certificate from a
  certificate authority. SmartScreen reputation still builds up over downloads.
  Sign both the installer and the bundled executables.

Until then, keep the first-launch guides linked from the release notes and
publish `SHA256SUMS` with every release.

## Signing setup and repository access

The initial Ed25519 public key is tracked in `packaging/keys/update-public.pem`.
The matching private key was generated locally at:

```text
.release-secrets/update-signing.pem
```

The directory is mode 0700 and the key is mode 0600. `.gitignore` excludes the
whole directory. Never attach the private key to a release, an issue, a build
artifact, or a commit. Keep an independent secure backup; losing it prevents
future releases from being trusted by these installed clients. Key rotation
needs a separately designed trust transition.

Before publishing, add the complete PEM file contents as the repository Actions
secret **MAGNITE_UPDATE_SIGNING_KEY** under Settings → Secrets and variables →
Actions. With an authenticated GitHub CLI, this can be done without displaying
its contents:

```sh
gh secret set MAGNITE_UPDATE_SIGNING_KEY < .release-secrets/update-signing.pem
```

The implementation environment currently has SSH access to the repository but
no GitHub API credential or installed `gh` CLI. Configuring this secret therefore
requires owner setup or an authenticated API session. The workflow reports a
clear error if signing is unconfigured; it does not publish unsigned updates.
Package-build artifacts remain available if building succeeded before that error.

The app contains no GitHub token. Public installed-app updates require public
release access. If the source repository remains private, use a public
release-distribution repository and configure `MAGNITE_RELEASE_REPOSITORY` at
build time. Publishing into a separate repository additionally requires a
credential with access to that repository; the current workflow publishes to
its own repository using its built-in token. Private testers can download from
GitHub after logging in, but this unauthenticated updater cannot read private
release assets.

Enable immutable releases in GitHub if desired; all assets are uploaded while
the release is still a draft. Do not enable settings that prevent the publishing
job from creating its intended preview tag without adapting the workflow.

## Linux update details (from v0.1-preview)

Settings → Updates shows the installed version, preview channel, check/download/
cancel/restart controls, and an automatic startup-check preference. Startup checks
run asynchronously after five seconds. Verification/selftest/screenshot modes
make no update check. Checking can fail offline without blocking local work.

The updater lists up to 100 releases, including prereleases, orders eligible
preview tags by version, and skips releases without Linux update assets. It does
not use GitHub's `/latest` endpoint, which excludes prereleases. It reads the
newest eligible Linux preview's
metadata and detached Ed25519 signature. It validates the signature before
trusting version, sequence, hash, size, download path, platform, CPU variant,
minimum glibc, or data compatibility. Download URLs must belong to the configured
repository and match the expected asset filename. HTTPS certificate checks stay
active, and redirects cannot downgrade to HTTP.

Downloads stream to an atomic save file in a private transaction directory beside
the installed image. The updater checks available disk space, a signed size bound,
the complete SHA-256, and the type-2 AppImage header. Cancellation or a failed
verification leaves the installed file unchanged. Downloads are complete bundles;
zsync/delta updates can be added after this first path is proven.

Installation is offered only for a writable Linux x86-64 AppImage. Source builds
can check releases but are rebuilt from source; extracted AppDirs and symlinked
or read-only images do not self-install. Put a portable AppImage in a permanent,
user-writable location before using automatic installation.

Restart is always explicit. The UI warns that current chat is not durable and
asks the user to finish/export work. The desktop disables interactions and asks
`POST /updates/quiesce` to close admission to new work. The ASGI gate rejects
quiescing while another HTTP operation, streamed response, background training
job, or model pull is active. If idle, it rejects new operations until shutdown.
`POST /updates/resume` cancels the maintenance state when necessary. This is
coordination, not a replacement for the separately deferred local IPC security.

The backend is terminated gracefully with a 15-second wait. The update path does
not force-kill it. If shutdown has not completed, installation is postponed.

The independent Linux helper is copied outside the mounted AppImage before exit.
It has no Qt/mounted-image dependency. A lock beside the installed AppImage
serializes application instances and installation. The helper waits for the old
application process and lock, then performs `renameat2(RENAME_EXCHANGE)` on the
same filesystem. This atomically exchanges complete files and retains old bytes.
It launches the new image and waits up to 120 seconds for the Qt app's backend-
readiness acknowledgement. A failed startup is stopped before restoring and
relaunching the previous image. The old image remains in the private transaction
directory after success, alongside status and installer logs. QSettings records
that directory as `updates/lastTransaction`.

A crash or power failure can leave a transaction directory requiring inspection.
The atomic exchange preserves both complete bundles; the helper does not promise
automatic recovery after a system reboot. A native desktop launch remains part
of acceptance testing, beyond the offscreen checks.

## User data and rollback limits

Updates replace the Qt client, bundled Python, backend and runtime together.
They do not reinstall Python packages into a developer venv or modify weights,
datasets, adapters, Qt preferences, or the Magnite store during installation.
Keep a writable AppImage separate from `MAGNITE_HOME` and model caches.

Existing stores have no versioned migration framework. The current updater
therefore requires an exact `DATA_SCHEMA` match. A release that needs a schema
change is rejected with migration instructions rather than installed silently.
The schema field is a release compatibility contract, not an implemented database
migration system. Maintainers must bump it when changing persistent formats.
A previous executable alone cannot reverse a database migration. Future migration
work must cover backups, interrupted migration, restored stores and downgrade
policy before loosening this restriction.

The first preview cannot demonstrate receiving a newer published version until
a subsequent version is published. Local tests exercise signed metadata and real
atomic helper installation/rollback with disposable launcher fixtures instead.

## Later work

AppImageUpdate/zsync deltas, `.deb`/`.rpm` repositories, Flatpak, staged rollout,
a stable channel (`v1.0.0` from `master`, see the
[multi-platform plan](MULTIPLATFORM_RELEASE_PLAN.md)), key rotation, a migration
framework for `DATA_SCHEMA` changes, and OS code signing.

## History: v0.1-preview implementation and verification log

- Inspected local/remote branches and existing architecture, release and packaging
  documentation; reused and advanced the original AppImage release branch.
- Centralized app, backend, CMake and package identities; added runtime constraints,
  verified deployment-tool hashes and a glibc compatibility audit.
- Added the preview-only Actions workflow and signed update metadata generation.
- Added Settings update controls, background checking, verified bounded downloads,
  maintenance admission, standalone atomic installation and startup rollback.
- Generated the initial local signing key; only its public key is tracked.
- CMake Release build: passed during implementation.
- Manifest/signature compatibility tests: passed during implementation.
- Theme audit: 0 failures. UI interaction/layout check: 0 failures, with loopback
  access outside the socket-restricted sandbox.
- Backend regressions, helper transaction checks, final package verification,
  GitHub secret setup and first hosted release: completed and recorded below.

Verification follow-up:

- All 58 backend regressions passed with local socket access. Sandboxed TestClient
  startup stalled; the passing run was outside that socket restriction.
- Six actual helper transaction tests passed: successful exchange/old-image
  retention, failed-startup rollback/relaunch, application-lock waiting, explicit
  cancellation, symlink rejection and unsafe staging permissions.
- A local AppImage rebuild uses an explicit newer-host diagnostic baseline;
  the Ubuntu/glibc-2.35 claim requires the hosted job, not this Arch build.


- The local AppImage was rebuilt successfully; the compatibility audit checked
  616 ELF files and found a highest glibc requirement of 2.43. Deployment tools
  and the embedded AppImage runtime are all verified against locked SHA-256 values.
- Superseded local AppImage/evidence were moved from `dist/` into
  `build/packaging/historical-pre-preview/`; historical binaries were not relabeled.
- Owner selected making the source repository public for unauthenticated updates.
  The owner configured the signing secret; hosted signing succeeded.

- Relocation testing caught stale Qt from the old AppDir. Native staging now
  resets before every build while preserving only the Python runtime cache;
  AppRun and native libraries are regenerated together.
- Removed the legacy CMake cached display-label override; canonical release
  identity remains the only editable application version.
- GitHub unauthenticated repository access was confirmed (HTTP 200).

- Added an explicit shell AppRun that executes the real `usr/bin/magnite-app`
  path, so Qt/runtime lookup does not depend on resolving a root-level launcher
  symlink through `/proc/self/exe`. Smoke tests force Qt diagnostics to stderr.

- All seven relocated package checks passed: extraction, theme, UI, desktop/IPC,
  real GGUF inference, combined CPU LoRA/adapter/RAG and the actual AppImage
  extract-and-run launcher. Logs are under `build/packaging/preview-verification/`.
- Signed local release metadata successfully and verified it against the tracked
  public key. This diagnostic Arch package is not the hosted Ubuntu artifact.
- Preserve extract-and-run mode when restarting an update, so a FUSE-free launch
  does not accidentally restart through a FUSE-only path.

## Published preview verification — 2026-10-23

- Release: [v0.1-preview](https://github.com/Ronit9320/magnite_llm/releases/tag/v0.1-preview), published as a prerelease,
  never marked latest stable. Source commit: `e629bfbf73b5ddc9d279f29dedccaf4544e64037`.
- [Hosted source verification](https://github.com/Ronit9320/magnite_llm/actions/runs/39993267083): passed on Ubuntu 24.04.
- [Hosted packaging and publication](https://github.com/Ronit9320/magnite_llm/actions/runs/39993267559): package and publish jobs passed
  on Ubuntu 22.04, including updater tests, backend regressions and relocated
  real tiny-model inference/LoRA/adapter/RAG workflows.
- Published AppImage: 422,869,496 bytes; SHA-256
  `1cac9a510fa6ddd22299399f838e4deae421604da122e93677c97927641de5b6`.
- Hosted audit: **597 ELF files, glibc 2.35 maximum requirement, zero offenders**.
  The public artifact is the Ubuntu-built package, not the local Arch diagnostic.
- Confirmed all six expected public release assets, tag, source commit and
  prerelease status. Manifest size/hash match GitHub's uploaded-asset digest;
  manifest compatibility matches the published ELF report.
- The actual Qt updater code, using bundled native libraries, fetched the public
  release and accepted its signature: **You are up to date · v0.1-preview**.
  This verifies live release discovery and trust; receiving a newer installed
  version still needs a subsequent preview release.
- The private signing key remained ignored and untracked. Keep its secure backup
  and the GitHub Actions secret for future releases.
- Implementation was committed and tested on `release/v0.1-testing`, then the
  verified release work and documentation were fast-forwarded into `dev` to
  activate future dev-triggered previews. `master` remains at `294c59a`; no master
  promotion was performed. Published assets are never overwritten. The dev
  integration can build again, but publication skips the already published version.

## References
- GitHub Releases API: https://docs.github.com/en/rest/releases/releases
- Immutable releases: https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases
- AppImage updates: https://docs.appimage.org/packaging-guide/optional/updates.html
- Qt Installer Framework: https://doc.qt.io/qtinstallerframework/ifw-updates.html
- Sparkle: https://sparkle-project.org/
- VS Code's Linux distribution: https://code.visualstudio.com/docs/setup/linux
