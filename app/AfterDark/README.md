# AfterDark (app)

SwiftUI macOS app: a sidebar module picker; the selected module runs in a
resizable/fullscreen window. **Every module runs the real emulated After Dark
code** via `EmulatedHost` (which drives the `engine` hosts) — there is no
native reimplementation or placeholder rendering of any module.

```bash
swift run AfterDark          # first launch downloads assets, then the picker
./make_app.sh                # or build a standalone AfterDark.app bundle
```

Global preferences: the **Duration** slider under the module list (the real
control panel's 13-stop ladder, "15 sec." to "Forever!") paces the screen
saver's Randomize rotation, exactly as the original Randomizer's Duration
did. A single selected module runs uninterrupted, also like the original. Caps Lock
reaches every module that listens for it (Time Flies changes clock type,
Rodger Dodger enters play mode, ...).

Targets: **AfterDarkKit** (EmulatedHost, catalog, first-run download, views),
**AfterDark** (the app), **adrender** (headless: `--smoke <module> <secs>` drives a
module through the full pipeline; `--verify` checks the catalog/recipes).
