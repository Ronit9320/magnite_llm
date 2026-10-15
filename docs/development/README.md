# Development documentation

Architecture, implementation plans, build processes, and technical reviews for
the app. Source paths and shell commands in these documents are relative to the
repository root unless stated otherwise.

| Document | Contents |
| --- | --- |
| [Architecture](architecture.md) | System boundaries, components, storage, and service interactions |
| [CMake build process](CMAKE_BUILD_PROCESS.md) | Desktop configuration, compilation, and build lifecycle |
| [Frontend analysis](FRONTEND_ANALYSIS.md) | Qt client structure and implementation review |
| [Backend process](BACKEND_PROCESS.md) | Startup, function calls, request flows, and shutdown |
| [Backend audit](BACKEND_AUDIT.md) | Findings, fixes, verification results, and remaining gaps |
| [September audit](AUDIT_2026-09-27.md) | Historical review and follow-up findings |

Local-only planning documents also live here when present: `app.md` and
`BUILD_PLAN.md`. They remain gitignored and are not available in fresh clones.
The [contribution rules](../../CONTRIBUTING.md) describe the finalized theme and
the feature branch → `dev` → `master` workflow.

For setup and contribution instructions, see [installation](../INSTALL.md) and
[CONTRIBUTING.md](../../CONTRIBUTING.md).
