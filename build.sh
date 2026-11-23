#!/usr/bin/env bash
#
# build.sh — build everything from a fresh clone, in one step:
#
#   1. the emulator dependency, at pinned revisions   (tools/build_deps.sh)
#   2. the two emulation hosts                        (engine/adhost, adhost68k)
#   3. the app                                        (app/AfterDark, SwiftPM)
#   4. the screen saver bundle                        (app/saver/AfterDark.saver)
#
# Prerequisites: an Apple Silicon Mac on macOS 14+, the Xcode Command Line Tools,
# and `brew install cmake unar hfsutils` (unar + hfsutils unpack the original
# After Dark files the app downloads on first launch).
#
#   usage: ./build.sh
#
# Safe to re-run: each step skips or rebuilds only what changed. The saver is
# ad-hoc signed, which is all a locally built bundle needs; set AD_SIGN_ID to
# sign with an identity from your keychain instead.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
ROOT="$PWD"

step(){ printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
die(){ printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "this project builds on macOS only"
[ "$(uname -m)" = arm64 ]  || die "Apple Silicon only (the screen saver bundle is built for arm64)"
swift --version >/dev/null 2>&1 \
  || die "swift is not usable — install the Command Line Tools (xcode-select --install), or if Xcode is installed accept its licence (sudo xcodebuild -license accept)"

step "1/4 emulator dependency (pinned)"
tools/build_deps.sh

step "2/4 emulation hosts"
make -C engine adhost adhost68k

step "3/4 app"
( cd app/AfterDark && swift build )

step "4/4 screen saver"
AD_SIGN_ID="${AD_SIGN_ID:--}" bash app/saver/build_saver.sh

missing=""
for t in unar hmount; do
  command -v "$t" >/dev/null 2>&1 || [ -x "/opt/homebrew/bin/$t" ] || missing="$missing $t"
done

cat <<MSG

Built. Next:

  1. Run the app once — it downloads the original After Dark modules
     (about 450 MB from archive.org) and is where you pick and preview them:

       cd "$ROOT/app/AfterDark" && swift run AfterDark

  2. Install the screen saver (macOS offers to install it), then choose
     "After Dark" in System Settings > Screen Saver:

       open "$ROOT/app/saver/AfterDark.saver"

MSG
if [ -n "$missing" ]; then
  echo "NOTE: the first-launch download needs tools that were not found:$missing"
  echo "      brew install unar hfsutils"
  echo
fi
