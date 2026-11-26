# Blueprints editor mockups

`editor-v2.html` is the interactive mockup for the Blueprints editor redesign
(November 2026). Open it directly in a browser; it loads the app's fonts from
`app/resources/fonts/`.

The app now ships this design (`BlueprintView`, the floating panels, the node
library, quick-add, the in-place inspector and the details window), so the
mockup is historical. Where the two differ, the app and its checks are the
reference.

It is a reference for the look and the interactions, not a specification:

- No borders: panels float over a dotted canvas and separate by shade and shadow.
- Nodes have a coloured header per family; pins and wires are coloured by data type.
- A node library dock on the left, a quick-add menu at the cursor (Space,
  right-click, double-click), and context-aware adding when a wire is dropped
  on empty canvas.
- A floating inspector, a bottom tool dock, a minimap, and a node details
  window opened from the ⓘ on nodes and node list entries.

The mockup predates the node decisions in `NODE_INVENTORY.md` (on
`feature/blueprint-workflow-nodes`): it still shows the Adapter node and
Format as text, which are leaving **Add node**. Its colours for data types and
node families are mockup values, not theme tokens yet.
