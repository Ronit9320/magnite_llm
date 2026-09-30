# Third-party assets and dependency inventory

The project MIT license covers Magnite's original material, not third-party
assets, libraries, model weights, or datasets. This inventory records what was
inspected; it is not a completed binary redistribution review.

## Bundled fonts

| Asset | Attribution | Included terms |
| --- | --- | --- |
| DM Sans | Copyright 2014 The DM Sans Project Authors | [SIL OFL 1.1](app/resources/fonts/OFL-DMSans.txt) |
| Space Grotesk | Copyright 2020 The Space Grotesk Project Authors | [SIL OFL 1.1](app/resources/fonts/OFL-SpaceGrotesk.txt) |

Copies of these fonts and OFL notices also appear under `design/midnight/fonts/`.
Keep the notices with redistributed font assets. Exact font provenance/version
matching should be confirmed before a binary bundle is published.

## Icons and historical design assets

`app/resources/icons/` contains 24 SVG files described by the project as
Lucide-style. Their SVGs do not carry origin/license notices. Styling or visual
similarity alone is insufficient proof of upstream provenance. **Release gate:**
confirm which icons were copied or adapted from upstream and include the exact
required attribution/license; identify original icons separately. No unverified
upstream license has been assigned to these files by this documentation pass.

Historical design material under `design/midnight/` includes SVG/PNG previews,
fonts, HTML, and a ZIP archive. It has been retained, not deleted or relicensed.
Review its provenance and whether it belongs in the public release.

## Linked and installed dependencies

Qt 6 Widgets, Network, and Svg are linked by the client. Qt terms depend on the
selected distribution/modules; review the actual bundle and preserve its notices
before shipping a binary. Source installation instructions do not constitute a
binary licensing review.

The table below transcribes license metadata from the installed direct Python
dependencies in the review environment on 2026-09-29. It is an environment
inventory, **not a lockfile, authoritative license interpretation, or complete
transitive SBOM**. Package contents may include separately licensed components.

| Direct dependency | Observed version | Installed metadata license |
| --- | --- | --- |
| fastapi | 0.141.1 | MIT |
| uvicorn | 0.52.4 | BSD-3-Clause |
| httpx | 0.28.1 | BSD-3-Clause |
| pydantic | 2.13.5 | MIT |
| huggingface_hub | 1.30.0 | Apache-2.0 |
| llama-cpp-python | 0.3.35 | MIT |
| tqdm | 4.70.0 | MPL-2.0 AND MIT |
| anyio | 4.15.0 | MIT |
| numpy | 2.5.2 | BSD-3-Clause AND 0BSD AND MIT AND Zlib AND CC0-1.0 |
| torch | 2.14.0 | Apache-2.0 AND Apache-2.0 WITH LLVM-exception AND BSD-2-Clause AND BSD-3-Clause AND BSL-1.0 AND MIT |
| transformers | 5.16.1 | Apache 2.0 License |
| peft | 0.20.0 | Apache |
| bitsandbytes | 0.50.2 | MIT |
| sentence-transformers | 6.0.1 | Apache-2.0 |
| datasets | 5.0.1 | Apache 2.0 |
| accelerate | 1.14.0 | Apache |
| safetensors | 0.8.0 | Apache Software License |
| pypdf | 6.19.0 | BSD-3-Clause |

Optional vLLM and Ollama are not installed by the declared backend extras; review
their selected distributions separately if they become bundled. Before binary
distribution, enumerate transitive/native libraries and include their applicable
license texts and notices. Do not copy this metadata table as a substitute.

## Models and example data

The repo's new `examples/` files are synthetic, original demonstration inputs;
see [their provenance](examples/README.md). Downloaded model weights, tokenizers,
embedding models, adapters derived from external models, and user datasets have
separate terms. Their presence in an app cache does not relicense them under MIT.
