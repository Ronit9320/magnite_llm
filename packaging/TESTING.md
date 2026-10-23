# Magnite v0.1-preview testing

This is an unfinished Linux x86-64 development preview. The broader application
release checklist is intentionally deferred while testing delivery and updates.

The package contains Qt, isolated Python, CPU llama.cpp/PyTorch, LoRA/PEFT and
RAG dependencies. It excludes model weights, CUDA, vLLM and Ollama. Initial model
and embedding downloads require internet access. CPU training suits small tests.

```sh
chmod +x Magnite-v0.1-preview-linux-x86_64.AppImage
./Magnite-v0.1-preview-linux-x86_64.AppImage
```

If FUSE is unavailable, add `--appimage-extract-and-run`. Extracted `AppRun` can
also launch the app, but only a writable original AppImage can self-update.
Store the AppImage in a permanent user-writable directory; symlinked images
and protected installation directories do not support in-app installation.

CI builds on Ubuntu 22.04 and audits the bundled ELF requirements against glibc
2.35. See the attached `linux-compatibility.json` for the artifact's actual
requirement. A local build from a newer host can require newer glibc. Historical
local test packages are superseded and are not this preview artifact.

Keep testing stores separate from existing work:

```sh
MAGNITE_HOME="$HOME/.magnite-testing" \
XDG_CONFIG_HOME="$HOME/.config/magnite-testing" \
HF_HOME="$HOME/.cache/magnite-testing-hf" \
./Magnite-v0.1-preview-linux-x86_64.AppImage
```

Without these overrides the package shares the normal application store and
model caches. Removing or updating the AppImage does not remove user data.
Chat is currently in memory: export/copy anything important before closing.

Settings → Updates checks publicly accessible GitHub preview releases. The first
preview cannot receive a newer update until a later preview is published. The
client verifies signed metadata and the complete download, then offers a restart.
Finish training, downloads, chat generation and document work first. Updates do
not force-kill the backend. The previous AppImage and an installation status log
are retained in a private `.magnite-update-*` directory beside the app.

The app accepts `--theme-check`, `--ui-check` and `--selftest`; use disposable data.
`build-info/` records source and dependencies inside the image. Existing security,
provenance, migration and clean-machine release work remains open; report observed
behavior with the exact version, commit, OS and compatibility report.
