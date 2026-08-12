# AfterDark (app)

SwiftUI macOS app + a headless renderer. A sidebar module picker; the selected
module runs in a resizable / fullscreen-capable window. Every module runs the
**real emulated After Dark code** through `EmulatedHost` (which drives the
`tools/adhost` hosts); there is no bespoke reimplementation.

## Run it
```
cd app/AfterDark
swift run AfterDark     # first launch shows the asset-download gate, then the picker
# or a standalone bundle:
./make_app.sh && open AfterDark.app
```
First run downloads the original assets (see the top-level README, *Assets*).

## Targets
- **AfterDarkKit** — `EmulatedHost` (drives a host, streams/decodes frames), the
  catalog loader, `FirstRunManager` (first-run asset download), the module views.
- **AfterDark** — the SwiftUI app.
- **adrender** — headless renderer / verifier. `adrender <module> <outPrefix> <w> <h>
  <t0,t1,…>` renders frames; `adrender --smoke <module> <secs>` drives a module
  through the full `EmulatedHost` pipeline and reports frames (recipe resolution +
  host path check); `adrender --verify` sanity-checks the catalog + recipes.
