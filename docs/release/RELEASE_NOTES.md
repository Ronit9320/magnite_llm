# Magnite v0.1-preview

First Linux x86-64 development preview, intended to test installation, local
workflows, and the new release/update pipeline. The app is unfinished and the
broader release checklist remains open by design for this preview.

The AppImage bundles the native Qt desktop, isolated Python runtime, CPU
llama.cpp/PyTorch, LoRA/PEFT and document retrieval dependencies. Model and
embedding weights are downloaded separately. No system Python installation is
required. CPU training is appropriate for small tests; this package does not
promise CUDA, QLoRA, vLLM, macOS or Windows support.

Make `Magnite-v0.1-preview-linux-x86_64.AppImage` executable and launch it from
a permanent user-writable location. If FUSE is unavailable, use
`--appimage-extract-and-run`. See the attached TESTING.md for isolated test data
and the compatibility report for the packaged ELF requirements. Ubuntu 22.04 is
the pipeline's initial build/test baseline; broader native desktop verification
is still pending.

Settings → Updates can check the preview channel, download a verified update,
and restart to install once active work is finished. Updates preserve application
data and model caches. The first release will show no newer version until another
preview is published. Update access requires publicly accessible release assets.

This preview retains known limitations, including unfinished local IPC security,
no durable chat history, no automatic training resume, no schema migration system,
and incomplete release licensing/provenance review. Use disposable test data;
keep backups of anything important. Automatic update installation rejects a
release that requires a different data compatibility generation.

Original Magnite code is MIT licensed. Bundled dependencies, fonts, models and
datasets retain their respective licenses. This is a prerelease and is never
marked as the latest stable release.
