# wf-csd-deco — groups, tabs and morphing 

> **Build target: bleeding-edge Wayfire (git master).** This variant is
> compiled against the current Wayfire development API, where `wf::geometry_t`
> is floating-point and scene-node damage/visibility use `wf::regionf_t`. The
> morph/drag machinery from the older working tree has been ported on
> top of that API. If you are on a tagged Wayfire release instead, use the
> pre-port tree — the scene-node signatures here (`regionf_t`) will not match
> an older `region_t`-based Wayfire.

A merge of two Wayfire projects:

- **wf-csd-deco-groups-n-tabs** — GTK4 client-side style decorations with
  drag-and-drop window grouping via the titlebar app icon.
- **wf-group-tab** — Compiz-style animation machinery (live 2D-transformed
  views), from which the morph is adapted.

## What it does

- **Drag to group** (now compositor-side): grab the app icon in a window's
  titlebar and drop it onto **any part of another window** to group the two.
  The client only tells the compositor a drag started (`start_group_drag`);
  the compositor dims the source, switches the cursor, tracks the pointer,
  and groups on release over whatever toplevel is underneath — GTK's own
  drag-and-drop proved unreliable on the decoration surfaces. Grouped
  windows stack on top of each other and share a tab strip of app icons in
  every member's titlebar. Middle-click a tab to ungroup it.

- **Morphing tab switch** (new): clicking a tab icon no longer snaps
  instantly to the other window. Instead the newly selected window fades in
  while morphing from the old window's exact geometry into its own, and the
  old window scales toward the new one's shape behind it — a seamless
  cross-morph. The old window is hidden when the animation completes.


## Tunables

Compile-time constants near the top of `wf-plugin/gtkdecor.cpp`:

| constant             | default | meaning                                 |
|----------------------|---------|-----------------------------------------|
| `morph_duration_ms`  | 300     | duration of the tab-switch morph        |

## Build

```
meson setup build
ninja -C build
sudo ninja -C build install
```

Then enable the `gtk4-decorator` plugin in your Wayfire config and run the
`wf-gtk4-decorator` client (e.g. autostart it). Both the plugin and the
client must be rebuilt together, since the protocol gained two requests.
- Previews only appear for *hidden* members of a group — hovering the tab
  of the currently visible window does nothing.
