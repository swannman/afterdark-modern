# Installing After Dark Modern

Requirements: Apple Silicon Mac (arm64), macOS 14 or later.

The download contains two things:

- **AfterDark.app** — the module picker. Runs any module in a window or
  fullscreen, and manages the one-time asset download.
- **AfterDark.saver** — the real macOS screen saver.

## 1. Open the app first

Move `AfterDark.app` somewhere permanent (e.g. `/Applications`), then open it.

Signed releases are notarized by Apple and open normally. If you grabbed an
unsigned CI artifact or built from source and macOS refuses to open it, either
**right-click the app → Open → Open** (macOS 14), approve it under System
Settings → Privacy & Security → "Open Anyway" (macOS 15+), or clear the
quarantine flag in Terminal:

```bash
xattr -dr com.apple.quarantine /Applications/AfterDark.app
```

On first launch the app downloads and verifies the original After Dark modules
from archive.org (they are not distributed with this project; you are
responsible for sourcing them legally). When the picker appears, the assets are
in place — this is also what the screen saver reads, so **run the app once
before using the saver**.

## 2. Install the screen saver

Double-click `AfterDark.saver` (clear its quarantine flag the same way if
macOS refuses) and macOS will offer to install it. Then pick **After Dark** in
System Settings → Screen Saver, and use its Options sheet to choose a module.

## Updating

Replace both bundles with the new versions; assets are kept and re-verified in
place under `~/Library/Application Support/AfterDarkModern`.
