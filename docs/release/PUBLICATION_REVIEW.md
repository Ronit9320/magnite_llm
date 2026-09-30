# Publication review — 2026-09-29

Reviewed the local repository at base commit `e5bc399`, before the new release
content was committed. No history was rewritten and no remote settings changed.

## Automated local inspection

- Enumerated all 12 commits reachable from local refs and inspected 284 unique blobs.
- Inspected the contents of one ZIP archive, with a total uncompressed-size guard.
- Searched for recognizable private-key headers, GitHub/HF/OpenAI-style tokens,
  AWS access-key IDs, credential-bearing HTTP URLs, and absolute home-directory paths.
- No matches for those patterns were found. Only finding types/paths would have
  been reported; no secret values were printed.
- Inspected the tracked path inventory: no downloaded model-weight files or user
  training/document stores were identified by name. Existing example text in tests
  is synthetic test material; new examples have explicit provenance.

This is a limited pattern/path review, not a claim that history is secret-free.
It does not detect every credential format, arbitrary secrets, image-embedded
content, externally stored data, or refs absent from the local repository. A
maintainer publication review and preferably a dedicated secret scanner remain
release gates. No remote history was fetched for this check.

## Asset/repository findings

- No project LICENSE existed at review start; MIT was added following the owner's choice.
- Font OFL notices exist. SVG icon provenance needs confirmation; see
  [third-party inventory](../../THIRD_PARTY_NOTICES.md).
- The historical `design/midnight/magnite-midnight.zip` is about 2.3 MB and
  `design/midnight/overview.png` about 1.3 MB. Retained pending a maintainer decision
  on historical assets; their size alone is not a reason to delete history.
- The origin is `Ronit9320/magnite_llm` on GitHub. Documentation uses that repository;
  its public visibility and security-reporting settings were not changed or verified.
- New screenshots use empty disposable stores/settings. They show preview/connecting
  states and do not demonstrate a loaded model or completed training session.

## Still required before publication

- Replace SECURITY.md's private-contact placeholder and define supported versions.
- Confirm icon and historical-asset provenance and final repository contents.
- Complete final history/content review and enable the chosen repository protections.
- Run the added CI workflow on GitHub; local checks do not establish hosted CI success.
- Re-run candidate checks after source changes and use the exact release commit/tag.

## Cleanup follow-up — 2026-10-01

The user requested removal of repository leftovers. Superseded Midnight and
Studio 1/2 artifacts, duplicate font copies, unused stylesheets and outdated
preview copies were removed. Current documentation links to Studio Motion
previews. Historical observations above describe the earlier snapshot; removed
artifacts remain in Git history.

Cleanup verification passed: CMake build, theme audit (0 failures), UI interaction
check (0 failures), 24 backend regressions and bare desktop selftest (0 failures).
Tracked Markdown links resolve, and `git diff --check` is clean. Loopback-dependent
checks ran outside the restricted sandbox. Real-model feature checks were not
repeated for this removal-only pass; see UI_POLISH.md for the prior results.
