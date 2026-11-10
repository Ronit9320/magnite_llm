# Development documentation

Architecture, implementation plans, build processes, and technical reviews for
the app. Source paths and shell commands in these documents are relative to the
repository root unless stated otherwise.

| Document | Contents |
| --- | --- |
| [Architecture](architecture.md) | System boundaries, components, storage, and service interactions |
| [CMake build process](CMAKE_BUILD_PROCESS.md) | Desktop configuration, compilation, and build lifecycle |
| [Frontend analysis](FRONTEND_ANALYSIS.md) | Qt client structure and implementation review |
| [Docs reference](DOCS_REFERENCE.md) | Offline article library, native reading tools, content maintenance and verification |
| [Settings window](SETTINGS_WINDOW.md) | Native settings, data actions, diagnostics, notifications and verification |
| [Training mockup workflow](TRAINING_MOCKUP_WORKFLOW.md) | Separate browser prototype, design review, screenshots and native translation |
| [Training visualisation](TRAINING_VISUALISATION.md) | Studio, Observatory, measured charts, configuration and verification |
| [Backend process](BACKEND_PROCESS.md) | Startup, function calls, request flows, and shutdown |
| [Backend audit](BACKEND_AUDIT.md) | Findings, fixes, verification results, and remaining gaps |
| [September audit](AUDIT_2026-09-27.md) | Historical review and follow-up findings |
| [Current documentation review](DOCUMENTATION_REVIEW.md) | Current-state corrections, source references, and remaining evidence gaps |
| [Native packages](NATIVE_PACKAGES.md) | Integrated Linux/Mac/Windows testing artifacts and hosted verification |
| [Multi-platform release plan](MULTIPLATFORM_RELEASE_PLAN.md) | Completed integration phases and remaining release work |
| [Release pipeline](RELEASE_PIPELINE.md) | Linux preview publication and signed updates |

Local-only planning documents also live here when present: `app.md` and
`BUILD_PLAN.md`. They remain gitignored and are not available in fresh clones.
The [contribution rules](../../CONTRIBUTING.md) describe the finalized theme and
the feature branch → `dev` → `master` workflow.

For setup and contribution instructions, see [installation](../INSTALL.md) and
[CONTRIBUTING.md](../../CONTRIBUTING.md).
