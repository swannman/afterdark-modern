#!/bin/bash
# Assemble AfterDark.saver — the shippable macOS screen saver. SwiftPM cannot emit a
# .saver, so we build the MH_BUNDLE Mach-O by hand (like the app's make_app.sh):
#
#   * PRINCIPAL CLASS (Swift): ADSaverView + the ported model/catalog, compiled
#     TOGETHER with the app's EmulatedHost.swift + ADPaths.swift (reused verbatim by
#     absolute path) + the ADCShim C shim into one bundle module.
#   * HOSTS bundled + signed in Contents/Resources (self-contained install).
#   * catalog.json copied into Contents/Resources.
#   * signed with the app-group entitlement so the app->saver asset handoff works.
#
# Env: AD_SIGN_ID overrides the signing identity.
set -euo pipefail
cd "$(dirname "$0")"

REPO="$(cd ../../ && pwd)"
KIT="$REPO/app/AfterDark/Sources/AfterDarkKit"
HOSTS_SRC="$REPO/engine"
SIGN_ID="${AD_SIGN_ID:--}"

NAME=AfterDark
OUT="$NAME.saver"
MACOS="$OUT/Contents/MacOS"
RES="$OUT/Contents/Resources"

echo "== assembling $OUT =="
rm -rf "$OUT"
mkdir -p "$MACOS" "$RES"
cp AfterDark-Info.plist "$OUT/Contents/Info.plist"

# --- ADCShim C shim (shm_open ABI wrapper EmulatedHost imports) ---------------
SDK="$(xcrun --show-sdk-path)"
clang -c -O2 -isysroot "$SDK" -mmacosx-version-min=14.0 -Icshim cshim/shim.c -o cshim/shim.o

# --- Compile the principal class + reused kit sources into the bundle Mach-O ---
# EmulatedHost.swift + ADPaths.swift + ADDuration.swift are the app's own files,
# single-sourced (they are deliberately SwiftUI-free so they compile into the
# MH_BUNDLE unchanged); ADSaverModel.swift / ADSaverView.swift are saver-local. `-Icshim` lets swiftc find
# the ADCShim clang module; shim.o is linked in. `-Xlinker -bundle` makes an
# MH_BUNDLE (an NSBundle-loadable principalClass), not an executable.
xcrun swiftc \
    -O -module-name ADSaver \
    -sdk "$SDK" \
    -target arm64-apple-macos14.0 \
    -Icshim \
    -framework ScreenSaver -framework AppKit -framework CoreGraphics -framework ImageIO \
    "$KIT/EmulatedHost.swift" \
    "$KIT/ADPaths.swift" \
    "$KIT/ADDuration.swift" \
    "$KIT/ADSharedSettings.swift" \
    ADSaverModel.swift \
    ADSaverView.swift \
    cshim/shim.o \
    -emit-executable -Xlinker -bundle \
    -o "$MACOS/$NAME"

# --- Resources: catalog + emulation hosts -------------------------------------
cp "$KIT/Resources/catalog.json" "$RES/catalog.json"

# Bundle the settled host binaries (NOT rebuilt — reuse as-is). Self-contained:
# they depend only on system dylibs.
cp "$HOSTS_SRC/adhost68k" "$HOSTS_SRC/adhost" "$RES/"
for h in adhost68k adhost; do
    extra="$(otool -L "$RES/$h" | tail -n +2 | grep -vE '^\s+/(usr/lib|System)/' || true)"
    [ -n "$extra" ] && { echo "WARNING: $h has non-system dylib deps:" >&2; echo "$extra" >&2; }
done

# --- Sign: hosts first (inner Mach-Os), then the bundle -----------------------
# A "Developer ID Application" identity signs for distribution: hardened runtime,
# trusted timestamp, and NO entitlements — get-task-allow fails notarization, and
# the app-group entitlement never took effect on an MH_BUNDLE anyway (the sidecar
# handoff is the real channel). Dev identities keep the entitlements so the
# debugger/harness can still attach.
case "$SIGN_ID" in
  "Developer ID"*) SIGN_OPTS=(--options runtime --timestamp); ENT=() ;;
  *)               SIGN_OPTS=(--options runtime --timestamp=none)
                   ENT=(--entitlements AfterDark.entitlements) ;;
esac
codesign --force --sign "$SIGN_ID" "${SIGN_OPTS[@]}" "$RES/adhost68k" "$RES/adhost" \
  || codesign --force --sign - "$RES/adhost68k" "$RES/adhost"
codesign --force --sign "$SIGN_ID" "${SIGN_OPTS[@]}" "${ENT[@]}" "$OUT" \
  || codesign --force --sign - "${ENT[@]}" "$OUT"

echo "== built $OUT =="
codesign -dvv "$OUT" 2>&1 | grep -E 'Identifier|Authority|TeamIdentifier' | head
file "$MACOS/$NAME"
