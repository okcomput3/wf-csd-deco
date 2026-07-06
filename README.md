# wf-csd-deco — groups, tabs, morphing & hover previews

> **Build target: bleeding-edge Wayfire (git master).** This variant is
> compiled against the current Wayfire development API, where `wf::geometry_t`
> is floating-point and scene-node damage/visibility use `wf::regionf_t`. The
> morph/preview/drag machinery from the older working tree has been ported on
> top of that API. If you are on a tagged Wayfire release instead, use the
> pre-port tree — the scene-node signatures here (`regionf_t`) will not match
> an older `region_t`-based Wayfire.

A merge of two Wayfire projects:

- **wf-csd-deco-groups-n-tabs** — GTK4 client-side style decorations with
  drag-and-drop window grouping via the titlebar app icon.
- **wf-group-tab** — Compiz-style animation machinery (live 2D-transformed
  views), from which the morph and preview effects here are adapted.

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

- **Live hover previews** (compositor support present, client trigger
  currently disabled): the compositor can pop up a live, scaled-down
  thumbnail of a hidden group member — centered above the visible window's
  top edge (or below it if there's no room), fading in/out in a dedicated
  overlay scene node in front of the workspace layer so it can never fall
  behind other windows. The plumbing (`preview_window` / `end_preview`
  requests and the `do_preview_window` / `do_end_preview` handlers) is all
  here, but the GTK4 client no longer attaches the hover motion controllers
  that fire it (see `/* Hover previews were removed. */` in `main.cpp`), so
  no thumbnail appears at runtime. Re-enable by attaching a
  `GtkEventControllerMotion` to each tab button and calling
  `preview_window(id)` / `end_preview(id)` on enter/leave.

## How it works

The GTK4 decorator client only knows about widgets; the compositor only
knows about views. Hovering therefore needed two new requests in the
private `wf_decorator` protocol:

```
preview_window(id)   — show a live thumbnail of hidden group member <id>
end_preview(id)      — fade the thumbnail out again
```

The client attaches a `GtkEventControllerMotion` to every tab button and
sends these on enter/leave. The compositor side implements the thumbnail
and the click-morph with `wf::scene::view_2d_transformer_t`
(scale/translate/alpha on the live view, decorations included), driven by
a per-frame `OUTPUT_EFFECT_PRE` hook with smoothstep easing — the same
technique as the wf-group-tab plugin.

## Tunables

Compile-time constants near the top of `wf-plugin/gtkdecor.cpp`:

| constant             | default | meaning                                 |
|----------------------|---------|-----------------------------------------|
| `morph_duration_ms`  | 300     | duration of the tab-switch morph        |
| `preview_fade_ms`    | 150     | preview fade in/out duration            |
| `preview_height_px`  | 200     | height of the hover thumbnail           |
| `preview_margin_px`  | 12      | gap between thumbnail and window edge   |

## Build

```
meson setup build
ninja -C build
sudo ninja -C build install
```

Then enable the `gtk4-decorator` plugin in your Wayfire config and run the
`wf-gtk4-decorator` client (e.g. autostart it). Both the plugin and the
client must be rebuilt together, since the protocol gained two requests.

## Notes

- Clicking a tab while a preview is showing cancels the preview and starts
  the morph; grouping, ungrouping and window destruction also cancel any
  in-flight animation safely.
- Previews only appear for *hidden* members of a group — hovering the tab
  of the currently visible window does nothing.
