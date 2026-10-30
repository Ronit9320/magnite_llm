# Magnite v0.1 testing build — macOS

This is an unfinished application packaged for testing, not a stable release. It is **not notarized**.

The app contains Qt, Python 3.12 and the backend, with llama.cpp (Metal), PyTorch (MPS), LoRA/PEFT and document-retrieval dependencies.
It does not contain model weights; the first model or embedding download needs internet access. vLLM and Ollama are not bundled.

**Requirements:** an Apple silicon Mac (M1 or later) running macOS 14 (Sonoma) or newer. Intel Macs are not supported. The build was
tested only on an M1 with 8 GB under macOS 27.0.1; other versions and chips are untested.

## Install

1. Open `Magnite-v0.1-preview-arm64.dmg` and drag **Magnite** into **Applications**. Eject the image.
2. Compare the download with the published checksum if you were given one:
   `shasum -a 256 Magnite-v0.1-preview-arm64.dmg`
3. The first launch of a downloaded copy is blocked by Gatekeeper because the app is not notarized. Follow the
   [first-launch guide](../../docs/MACOS_FIRST_LAUNCH.md) (System Settings → Privacy & Security → Open Anyway). You only do this once.

Please write down, or screenshot, exactly what each dialog says. The wording has not been recorded yet.

## What to expect

- **The first model load can pause for about 10 seconds** while macOS prepares GPU shaders. This happens once per macOS user. If you see a
  pause, note how long it lasted and whether it was on loading the model or on the first reply.
- Chat with GGUF models runs on the GPU (Metal). LoRA training runs on the GPU (MPS) and is much faster than on the CPU.
- **4-bit and 8-bit QLoRA are not available on macOS.** The option is disabled in Training.
- Tested with models up to about 1.5B parameters. Larger models have not been tried.

## Keep test data separate

Application data lives in `~/.magnite`. To test without touching an existing install, start the app from Terminal with another folder:

```sh
MAGNITE_HOME="$HOME/.magnite-testing" /Applications/Magnite.app/Contents/MacOS/Magnite
```

Hugging Face caches are separate; set `HF_HOME` to isolate downloaded weights as well.

Magnite refuses to start a second copy on the same data folder and shows a message saying it is already running.

## Quitting and crashes

Quit the app normally (the Quit menu item or ⌘Q). The backend stops with the app. If the app is force-quit or crashes, the backend should exit within a
second or two. If something doesn't look right, open Activity Monitor and search for `python3`; a process whose path contains
`Magnite.app/Contents/Resources/runtime` can be quit by hand. Please report it.

## Uninstall

Drag **Magnite** from Applications to the Trash. This does **not** remove your data:

- `~/.magnite`: models registry, adapters, datasets, training history, RAG index (your Hugging Face cache is separate);
- the preferences domain **`com.magnite-local.Magnite LLM`** (remove with `defaults delete "com.magnite-local.Magnite LLM"`).

Delete those yourself for a complete removal.

## Smoke tests

The app accepts `--theme-check`, `--ui-check` and `--selftest`. Run them against a disposable `MAGNITE_HOME`:

```sh
MAGNITE_HOME="$(mktemp -d)" /Applications/Magnite.app/Contents/MacOS/Magnite --selftest
```

A pass prints `[selftest] PASS (0 failures)`. The repository's `packaging/macos/smoke_bundle.sh <app>` runs the full automated check and
confirms that the app did not modify its own bundle.

`Contents/Resources/build-info/build.json` records the source commit, whether the tree was modified, and the runtime. Licence texts for
Qt and third-party components are in `Contents/Resources/licenses/`. See the
[verification record](../../docs/release/macos/V0_1_MACOS_TESTING.md) for what has and has not been verified.
