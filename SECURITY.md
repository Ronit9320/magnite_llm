# Security policy

## Reporting a vulnerability

**Maintainer action required before public release:** replace
`[PRIVATE_SECURITY_CONTACT_PENDING]` with a monitored private email address or a
confirmed GitHub private vulnerability reporting URL.

The reporting channel has not yet been configured. Do not post exploit details,
credentials, private datasets, or personal filesystem contents in public issues.
No private reporting endpoint or response-time commitment is implied by this draft.

When the channel is available, include the affected commit/version, operating
system, reproduction steps using synthetic inputs, impact, and any proposed fix.

## Supported versions and scope

Magnite is in pre-release development; no stable release support window is
established yet. Reports should identify the exact revision. Before publishing,
maintainers must designate supported versions and a monitored reporting route.

The backend is intended for the local desktop and binds to 127.0.0.1. It currently
has no application authentication and exposes local-path and mutation operations.
Do not expose it as a public or LAN service. Application data is not encrypted
by Magnite. Treat model files, adapters, datasets, and documents as untrusted inputs.

Publication gates are tracked in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md).
