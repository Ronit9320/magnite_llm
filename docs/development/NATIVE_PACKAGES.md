# Native package builds and integration verification

**Verified 2026-11-06:** [run 42117841134](https://github.com/Ronit9320/magnite_llm/actions/runs/42117841134)
passed all three native jobs and the combined artifact gate from clean source
`57d6be0125905f8cbd1d3ddbee3abd7713a37f0b`, including the latest Mac app fixes
through `2164dc7` and integration review fixes. Shared verification passed too.
Download the platform `*-preview-*` testing artifacts from that run; evidence
artifacts contain environment, resolved dependencies and test logs. GitHub may
require login for downloads. No release is needed to test these artifacts.
Checksums and provenance are included and verified by the combined gate.
The existing public `v0.1-preview` assets remain unchanged. Follow-up results
and the earlier baseline run `41081185134` are recorded in
[the release plan](MULTIPLATFORM_RELEASE_PLAN.md).

`native-packages.yml` is an **artifact-only** coordinator for pushes to
`dev`/`master`, PRs targeting those branches, and manual runs on `release/**`.
Automatic jobs use positive path filters for app, backend, packaging, workflows,
license and third-party notices. Ordinary documentation-only pushes skip them;
packaged documentation under `packaging/` still triggers checks. Working branches
such as `mac` do not run on push. Keep the central workflow files tracked rather
than deleting them on each platform branch. It has read-only
repository permission, no publishing job, and no release signing secrets.
The GitHub UI's **Run workflow** button requires the workflow file on the
repository default branch (`master` at this inspection). Until the workflows
reach that branch, use a PR targeting `dev` for automatic validation, or an
authenticated API/CLI dispatch of the already registered workflow. See
[GitHub's workflow_dispatch reference](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#workflow_dispatch).
No default-branch setting is changed by this work.

It checks one source SHA and passes that exact SHA to reusable native jobs:

| Job | Native runner | Output |
| --- | --- | --- |
| `build-linux.yml` | Ubuntu 22.04 x86-64 | CPU AppImage, glibc ≤ 2.35 |
| `build-macos.yml` | macOS 15 ARM (`uname -m` must be arm64) | Ad-hoc-signed app in a DMG, declared macOS 14 minimum |
| `build-windows.yml` | Windows Server 2022 x64, MSVC 2022 | Unsigned per-user CPU installer |

Runner labels follow the
[GitHub runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
The Mac job intentionally asserts its architecture rather than silently building
Intel if the runner label ever changes meaning. No Intel Mac package is claimed.
The application version/sequence/schema are not changed by package-only testing.

Each job builds from a clean checkout, stages a fresh native application,
refreshes the backend wheel, runs package-specific checks, and uploads the
exact tested package with platform-specific checksums and provenance. The
provenance contains canonical version, monotonic sequence, data schema, source
commit, architecture, host environment and resolved Python packages. The
final acceptance job checks every required platform, identity, clean-source
claim, size and full SHA-256. A missing or failed platform blocks that job.
These manifests are build evidence, not the signed update metadata of Phase 5/6.

Linux retains its pinned AppImage tools and portable CPU flags. The existing
Linux preview workflow calls the same reusable builder and is manual-only.
Its `publish` input defaults to false; only an explicit opt-in on `dev` can publish
through that legacy Linux-only path. Normal verification/native CI has no publisher.
The coordinated publisher remains Phase 6 work. Release branches never publish.

Mac and Windows host tooling uses setup-python 3.12.10 (3.12.12 is unavailable
there); their isolated shipped runtime remains Python 3.12.12 through uv.
Mac uses official Qt 6.8.3 installed by aqtinstall 3.3.0. The updater's OpenSSL
3.6.5 is checksum-pinned and built statically for macOS 14; it does not inherit
a newer Homebrew bottle's deployment baseline. Existing bundle/Mach-O audits,
poisoned-environment smoke, UI, backend regressions, CPU feature workflow and
DMG mount smoke run natively. Metal/MPS capability claims still require a real
device; hosted CPU tests do not close that acceptance gap. Developer ID signing
and notarization are not part of these ad-hoc testing artifacts.

On GitHub's Mac account, upstream wheel diagnostics and SBOMs may contain the
same generic `/Users/runner` prefix as the current host. The string audit allows
that generic prefix only in prebuilt third-party binary/archive or dist-info
data; it still rejects exact checkout/Qt roots and host paths in scripts.
Loader paths, dependency resolution, signatures and poisoned-runtime smoke
are audited independently. Local personal home prefixes remain fully checked.

Smoke log and data directories are resolved to absolute paths before launch.
The desktop also resolves relative `MAGNITE_HOME` values against its launch cwd,
before changing the backend cwd, so a relative override cannot accidentally put
databases inside the application bundle. Mac regression child interpreters
disable bytecode output, and bundle signatures are verified after regressions
and after relocated startup and a real tiny-model pull. Download workers preserve
isolated Python flags and disable bytecode writes. Content/link/mode inventories diagnose mutations;
OS-added provenance attributes are recorded separately from file contents.

Windows tooling and installer operation are in
[the platform guide](../../packaging/windows/README.md). Qt's deployment tool
does not supply every non-Qt dependency, so OpenSSL DLLs are staged explicitly;
see [Qt Windows deployment](https://doc.qt.io/qt-6.8/windows-deployment.html).

No staged app or Python installation is restored from a cache. Initial native
runs resolve dependencies only from the platform's pinned locks. uv's local
download/build cache is scoped to each job, and the backend wheel is refreshed
even if a local runtime is resumed. Runner disk-before/after, tool versions,+build logs, regressions and package evidence are uploaded even on failure when
they exist. Actions job timing supplies initial duration measurements.

For native failure investigation, inspect the named platform evidence artifact
and failed job step. Public job status can be read without a token; GitHub may
require authentication to download full logs/artifacts. A failure in automation
or absent native evidence leaves the relevant release-plan phase IN PROGRESS.
Workflow files alone do not establish native acceptance or Phase 3 completion.

## Integrating app work into dev

Phases 0–3 provide the native build/test foundation. Once the current integration
commit passes verification and all three native jobs plus the complete-set gate,
it can be reviewed and merged into `dev`. Phases 4–9 are prerequisites for their
respective update, coordinated publication and stable rollout claims, not for
ordinary development on `dev`. This change does not merge `dev` or `master`.

After integration lands, branch normal interface/backend work from updated `dev`,
for example `feature/library-interface`. Open a PR back to `dev`; its target branch
triggers the checks. Reserve `release/*` for release engineering, then promote
reviewed and tested `dev` to `master` when ready. Do not develop directly on either.
