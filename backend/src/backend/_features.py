"""Build-time feature profile; consumed by CMake, packaging and the backend.

Release branches may switch features off that are still being developed on
`dev`. Switching a feature off removes its UI, HTTP routes and bundled
runtime pieces; its code stays in the tree so turning it back on later needs
no data migration. Keep each value a plain `True`/`False` literal: CMake reads
this file with a regular expression.
"""

# Blueprints, Tool mode, and the bundled Node.js + Pi worker that both use.
BLUEPRINTS_AND_TOOLS = True
