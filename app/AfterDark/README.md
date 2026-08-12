# AfterDark (native app)

SwiftUI + SpriteKit macOS app. A sidebar module picker; the selected module runs
in a resizable / fullscreen-capable window. Flying Toasters is implemented with
the real decoded sprites; other modules show a placeholder for now.

## Run it
```
cd app/AfterDark
./make_app.sh          # builds AfterDark.app (a real launchable bundle)
open AfterDark.app     # pick a module in the sidebar; resize / green-button fullscreen
```
A bare `swift run` won't show a window (SwiftUI needs an .app bundle) — use `make_app.sh`.

## Targets
- **AfterDarkKit** — scenes, sprite loader, decoded sprite resources.
- **AfterDark** — the SwiftUI app.
- **adrender** — headless Metal renderer (`adrender <module> <outPrefix> <w> <h> <t0,t1,…>`),
  used to verify/preview scenes without a display.
