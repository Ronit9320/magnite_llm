# ADR 0005 — Signing: ad-hoc now, Developer ID deferred

- Date: 2026-10-23
- Status: Accepted (Developer ID work BLOCKED). Update 2026-10-30: the maintainer deferred Developer ID/notarization indefinitely; users approve the app through Privacy & Security, documented in `docs/MACOS_FIRST_LAUNCH.md`.

## Context

`security find-identity -v -p codesigning` on the build host reports
`0 valid identities found`; there is no Apple Developer Program enrollment yet.
`xcrun notarytool` is present (1.0.0 (28)), so tooling is not the obstacle —
credentials are.

Two separate facts the maintainer asked about: notarization is **not** the App
Store. Developer ID signing plus notarization is exactly what allows a `.dmg`
served from your own website to open without a warning. Without it, a downloaded
bundle carries `com.apple.quarantine` and macOS refuses it on first launch
("cannot be opened because Apple cannot check it for malicious software", or
"is damaged" when there is no signature at all), and the user must clear it via
System Settings → Privacy & Security → **Open Anyway**. On arm64, binaries with
no signature at all — not even ad-hoc — are rejected by the kernel outright.

## Options

1. Ship with no signature — arm64 will refuse to execute parts of the bundle.
2. **Ad-hoc sign (`codesign -s -`)** — satisfies the arm64 requirement that
   every Mach-O be signed, still triggers Gatekeeper on download.
3. Developer ID + notarization — impossible today, no certificate.

## Decision

Ad-hoc sign every Mach-O in the bundle (inner binaries first, then the bundle),
label the artifact explicitly as an **unsigned testing build**, and ship
Gatekeeper instructions with it. Keep Developer ID signing and notarization as
Phase 11, status BLOCKED.

Unblocking Phase 11 requires, exactly:

1. Apple Developer Program enrollment.
2. A **Developer ID Application** certificate and private key in the login
   keychain, visible to `security find-identity -v -p codesigning`.
3. The Team ID.
4. An app-specific password stored with
   `xcrun notarytool store-credentials magnite-notary --apple-id <id> --team-id <team> --password <app-specific>`.

## Consequences

- The macOS public-distribution checklist line stays unticked for v0.1.
- Every artifact name, `TESTING.md`, release note and checklist entry must say
  unsigned/un-notarized testing build. A test package is not release readiness.
- When unblocked, signing will need the entitlements
  `com.apple.security.cs.disable-library-validation` and
  `com.apple.security.cs.allow-unsigned-executable-memory`, because the bundle
  loads Python extension modules and ML code that hardened runtime otherwise
  refuses.
