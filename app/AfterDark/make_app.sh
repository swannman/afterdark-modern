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
# falls back to a dev checkout's tools/adfetch. verify_manifest.SH (not the .py
# twin) is what ships: the download path must not need python3.
ADFETCH_SRC="$(cd ../../tools/adfetch && pwd)"
mkdir -p "$APP/Contents/Helpers/adfetch"
cp "$ADFETCH_SRC/adfetch.sh" "$ADFETCH_SRC/sources.conf" \
   "$ADFETCH_SRC/assets.manifest.json" "$ADFETCH_SRC/verify_manifest.sh" \
   "$ADFETCH_SRC/manifest.sh" \
   "$APP/Contents/Helpers/adfetch/"

# Bundle the EXTRACTION TOOLS adfetch drives (unar + hfsutils' hmount/hcd/hls/
# hcopy/humount), built from source by tools/get_extractors.sh, so a downloaded
# .app has ZERO Homebrew prerequisites on first run. FirstRunManager points
# AD_TOOLS_DIR at this directory, which adfetch prepends to PATH.
# Licenses + pinned upstream sources: THIRD_PARTY_LICENSES.md.
EXTRACTORS="$(cd ../.. && pwd)/third_party/extractors/bin"
if [ ! -x "$EXTRACTORS/unar" ] || [ ! -x "$EXTRACTORS/hmount" ]; then
  echo "Building bundled extraction tools (unar, hfsutils)…"
  "$(cd ../../tools && pwd)/get_extractors.sh"
fi
mkdir -p "$APP/Contents/Helpers/bin"
for t in unar hmount hcd hls hcopy humount; do
  [ -x "$EXTRACTORS/$t" ] || { echo "ERROR: missing extractor $EXTRACTORS/$t" >&2; exit 1; }
  cp "$EXTRACTORS/$t" "$APP/Contents/Helpers/bin/$t"
done

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
for h in adhost68k adhost bin/unar bin/hmount bin/hcd bin/hls bin/hcopy bin/humount; do
  extra="$(otool -L "$APP/Contents/Helpers/$h" | tail -n +2 \
            | grep -vE '^\s+/(usr/lib|System)/' | grep -v ':$' || true)"
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

# Sign every Mach-O we bundle (hosts + extraction tools). AD_SIGN_ID overrides
# the identity (same convention as saver/build_saver.sh); with no usable identity
# we fall back to an ad-hoc signature, which is all a local dev build needs.
#
# The .app itself is NOT sealed here: codesign refuses the SwiftPM resource
# bundle in Contents/MacOS ("bundle format unrecognized" — it is a flat bundle
# with resources at its root), so an app-level signature needs that layout
# reworked first. Developer-ID + hardened runtime + notarization are the same
# open distribution follow-up.
SIGN_ID="${AD_SIGN_ID:--}"   # "-" = ad-hoc; set AD_SIGN_ID to your "Apple Development: ..." identity to sign properly
sign(){ codesign --force --sign "$SIGN_ID" --timestamp=none "$@" 2>/dev/null \
        || codesign --force --sign - "$@"; }
for h in "$APP/Contents/Helpers"/adhost68k "$APP/Contents/Helpers"/adhost \
         "$APP/Contents/Helpers/bin"/*; do
  sign "$h"
  codesign --verify --strict "$h" || { echo "ERROR: $h failed signature verify" >&2; exit 1; }
done
echo "codesign: hosts + extraction tools signed ($SIGN_ID)"

echo "Built $PWD/$APP"
echo "Run it with:  open '$PWD/$APP'"
