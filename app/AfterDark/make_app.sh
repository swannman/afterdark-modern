#!/bin/bash
# Assemble AfterDark.app from the SwiftPM build so it launches as a real windowed
# app (a bare SwiftPM executable won't show a window). Usage: ./make_app.sh
set -euo pipefail
cd "$(dirname "$0")"

CONFIG=${1:-debug}
swift build $([ "$CONFIG" = release ] && echo -c release)
BIN=".build/$CONFIG"

APP="AfterDark.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BIN/AfterDark" "$APP/Contents/MacOS/AfterDark"
# Bundle.module resolves the resource bundle relative to the executable's dir.
cp -R "$BIN/AfterDark_AfterDarkKit.bundle" "$APP/Contents/MacOS/" 2>/dev/null || \
  cp -R "$BIN"/*_AfterDarkKit.bundle "$APP/Contents/MacOS/"

# Bundle the first-run downloader (adfetch + its manifest/verify/sources) inside
# the app so a distributed .app can acquire the original assets on first launch.
# FirstRunManager looks here first (Contents/Helpers/adfetch/adfetch.sh), then
# falls back to a dev checkout's tools/adfetch. NOTE: the extract tools it drives
# (unar, hfsutils' hmount/hcopy) are NOT vendored here — they're detected on PATH
# and, if absent, the app shows a `brew install unar hfsutils` step. Vendoring +
# codesigning those helpers for a sandboxed/notarized build is a known follow-up.
ADFETCH_SRC="$(cd ../../tools/adfetch && pwd)"
mkdir -p "$APP/Contents/Helpers/adfetch"
cp "$ADFETCH_SRC/adfetch.sh" "$ADFETCH_SRC/sources.conf" \
   "$ADFETCH_SRC/assets.manifest.json" "$ADFETCH_SRC/verify_manifest.py" \
   "$APP/Contents/Helpers/adfetch/"

# Bundle the emulation HOST binaries (adhost68k = 68K, adhost = PPC) inside the
# .app so a shipped app can actually RUN a module — ADPaths.hostsDir resolves them
# from Contents/Helpers when distributed, else the dev tree. Build them FRESH from
# the current host sources via the tools/adhost Makefile (same clang flags/deps),
# so the bundled binaries always match source. The COPYRIGHTED shared libs
# (ADShared40.pef / AD4Library.rsrc) are deliberately NOT bundled — they arrive via
# the first-run download and are read from assets/shared at runtime.
ADHOST_SRC="$(cd ../../tools/adhost && pwd)"
echo "Building emulation hosts (adhost68k, adhost)…"
make -C "$ADHOST_SRC" adhost68k adhost
cp "$ADHOST_SRC/adhost68k" "$ADHOST_SRC/adhost" "$APP/Contents/Helpers/"
# Sanity: the bundled hosts must be self-contained (only system dylibs); a
# non-/usr/lib, non-/System dependency would need bundling too. Flag if present.
for h in adhost68k adhost; do
  extra="$(otool -L "$APP/Contents/Helpers/$h" | tail -n +2 \
            | grep -vE '^\s+/(usr/lib|System)/' || true)"
  if [ -n "$extra" ]; then
    echo "WARNING: $h has non-system dylib dependencies (would need bundling):" >&2
    echo "$extra" >&2
  fi
done

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>After Dark</string>
  <key>CFBundleDisplayName</key><string>After Dark 4.0</string>
  <key>CFBundleIdentifier</key><string>com.local.afterdark</string>
  <key>CFBundleVersion</key><string>0.1</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>AfterDark</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

echo "Built $PWD/$APP"
echo "Run it with:  open '$PWD/$APP'"
