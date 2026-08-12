# AfterDark (app)

SwiftUI macOS app: a sidebar module picker; the selected module runs in a
resizable/fullscreen window. Every module runs the real emulated After Dark code
via `EmulatedHost` (which drives the `tools/adhost` hosts).

```bash
swift run AfterDark          # first launch downloads assets, then the picker
./make_app.sh                # or build a standalone AfterDark.app bundle
```

Targets: **AfterDarkKit** (EmulatedHost, catalog, first-run download, views),
**AfterDark** (the app), **adrender** (headless: `--smoke <module> <secs>` drives a
module through the full pipeline; `--verify` checks the catalog/recipes).
