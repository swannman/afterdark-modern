#!/usr/bin/env bash
#
# get_extractors.sh — build the two archive-extraction tools adfetch drives
# (unar and hfsutils) from source into third_party/extractors/bin, so
# AfterDark.app can bundle them and a downloaded .app needs NO Homebrew.
#
# Both are built as UNIVERSAL (arm64 + x86_64) binaries against the same
# deployment target as the app. Nothing is committed: third_party/ is outside
# git's view; the repo stays source-only.
#
#   usage: tools/get_extractors.sh [--force] [--arch native|universal]
#
#   --force    rebuild even if the binaries are already present
#   --arch     universal (default) or native (faster; single-arch)
#
# WHAT IS BUILT (pinned; the URLs + tags below are the source-availability
# statement for the LGPL/GPL tools we redistribute — see THIRD_PARTY_LICENSES.md)
#
#   unar          XADMaster v1.10.8         LGPL-2.1-or-later
#                 + universal-detector 1.1  MPL/GPL/LGPL tri-license
#   hmount/hcd/hls/hcopy/humount
#                 hfsutils 3.2.6            GPL-2.0-or-later
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

XAD_TAG="v1.10.8"
UD_TAG="1.1"
HFS_VER="3.2.6"
HFS_URL="https://ftp.mars.org/hfs/hfsutils-${HFS_VER}.tar.gz"
HFS_URL_MIRROR="https://ftp2.osuosl.org/pub/clfs/conglomeration/hfsutils/hfsutils-${HFS_VER}.tar.gz"
HFS_SHA256="bc9d22d6d252b920ec9cdf18e00b7655a6189b3f34f42e58d5bb152957289840"
DEPLOY_TARGET="13.0"

# hfsutils ships ONE binary that dispatches on argv[0]; adfetch drives five of
# its names (hmount/hcd/hls/hcopy/humount), so five copies get installed.
HFS_TOOLS=(hmount hcd hls hcopy humount)
WANT=(unar "${HFS_TOOLS[@]}")

OUT="$ROOT/third_party/extractors"
SRC="$OUT/src"; BUILD="$OUT/build"; BIN="$OUT/bin"

FORCE=0; ARCHMODE=universal
while [ $# -gt 0 ]; do
  case "$1" in
    --force) FORCE=1; shift;;
    --arch) ARCHMODE="$2"; shift 2;;
    -h|--help) sed -n '2,26p' "$0"; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

log(){ printf '\033[1;35m[extractors]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[extractors] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

if [ "$ARCHMODE" = universal ]; then ARCHS="arm64 x86_64"; else ARCHS="$(uname -m)"; fi

have_all(){
  local t
  for t in "${WANT[@]}"; do [ -x "$BIN/$t" ] || return 1; done
  return 0
}

if [ "$FORCE" = 0 ] && have_all; then
  log "already built — $BIN"
  for t in "${WANT[@]}"; do printf '  %-8s %s\n' "$t" "$(lipo -archs "$BIN/$t" 2>/dev/null || echo '?')"; done
  exit 0
fi

command -v git >/dev/null || die "need git"
command -v xcodebuild >/dev/null || die "need Xcode (xcodebuild) to build unar"
mkdir -p "$SRC" "$BUILD" "$BIN"

# clone_at <url> <dir> <tag>  — idempotent shallow clone pinned to a tag. The
# stamp records what we cloned: XADMaster carries a stray tag pointing at the
# same commit as the release tag, so `git describe` can't answer this.
clone_at(){
  local url="$1" dir="$2" tag="$3"
  if [ -d "$dir/.git" ] && [ "$(cat "$dir/.pinned-tag" 2>/dev/null)" = "$tag" ]; then
    log "have $(basename "$dir") @ $tag"; return
  fi
  rm -rf "$dir"
  log "cloning $(basename "$dir") @ $tag"
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" "$url" "$dir"
  echo "$tag" > "$dir/.pinned-tag"
}

# ---------------------------------------------------------------- unar --------
build_unar(){
  clone_at https://github.com/MacPaw/XADMaster.git "$SRC/XADMaster" "$XAD_TAG"
  # The XADMaster Xcode project resolves UniversalDetector as a SIBLING directory.
  clone_at https://github.com/MacPaw/universal-detector.git "$SRC/UniversalDetector" "$UD_TAG"

  # The project still names the pre-libc++ standard library; modern toolchains
  # have no libstdc++.6.dylib (same fix Homebrew's unar formula applies).
  if grep -q 'libstdc++.6.dylib' "$SRC/XADMaster/XADMaster.xcodeproj/project.pbxproj"; then
    log "patching libstdc++.6.dylib -> libc++.1.dylib"
    /usr/bin/sed -i '' 's/libstdc++\.6\.dylib/libc++.1.dylib/g' \
      "$SRC/XADMaster/XADMaster.xcodeproj/project.pbxproj"
  fi
  # __DATE__ in the version banner would make every build differ.
  /usr/bin/sed -i '' 's/@__DATE__/@"Jan  1 2020"/' "$SRC/XADMaster/unar.m" 2>/dev/null || true

  local sym="$BUILD/xad"
  local t
  for t in XADMaster unar; do
    log "xcodebuild $t ($ARCHS)"
    ( cd "$SRC/XADMaster" && xcodebuild -project XADMaster.xcodeproj -target "$t" \
        -configuration Release "SYMROOT=$sym" \
        "MACOSX_DEPLOYMENT_TARGET=$DEPLOY_TARGET" "ARCHS=$ARCHS" ONLY_ACTIVE_ARCH=NO \
        CODE_SIGNING_ALLOWED=NO >"$BUILD/xad-$t.log" 2>&1 ) \
      || { tail -30 "$BUILD/xad-$t.log" >&2; die "unar build failed ($t) — full log $BUILD/xad-$t.log"; }
  done
  [ -x "$sym/Release/unar" ] || die "unar not produced in $sym/Release"
  install -m 755 "$sym/Release/unar" "$BIN/unar"
  log "installed $BIN/unar ($(lipo -archs "$BIN/unar"))"
}

# ------------------------------------------------------------ hfsutils --------
build_hfsutils(){
  local tgz="$BUILD/hfsutils-$HFS_VER.tar.gz"
  if [ ! -f "$tgz" ]; then
    log "downloading hfsutils $HFS_VER"
    curl -fsSL --retry 3 -o "$tgz" "$HFS_URL" \
      || curl -fsSL --retry 3 -o "$tgz" "$HFS_URL_MIRROR" \
      || die "could not download hfsutils from $HFS_URL"
  fi
  local got; got="$(shasum -a 256 "$tgz" | awk '{print $1}')"
  [ "$got" = "$HFS_SHA256" ] || die "hfsutils tarball sha256 $got != pinned $HFS_SHA256"

  local dir="$SRC/hfsutils-$HFS_VER"
  rm -rf "$dir"
  tar xzf "$tgz" -C "$SRC"

  # 1998-era C against a 2020s clang: implicit int / implicit function
  # declarations are errors now, and hpwd.c calls strcmp with no <string.h>
  # (the same two workarounds Homebrew's hfsutils formula carries).
  /usr/bin/sed -i '' 's|# include <stdio.h>|# include <stdio.h>\
# include <string.h>|' "$dir/hpwd.c"

  local cflags="-Wno-implicit-int -Wno-implicit-function-declaration -Wno-deprecated-non-prototype"
  local a; for a in $ARCHS; do cflags="$cflags -arch $a"; done
  cflags="$cflags -mmacosx-version-min=$DEPLOY_TARGET"

  log "configure hfsutils ($ARCHS)"
  ( cd "$dir" && CFLAGS="$cflags" LDFLAGS="$cflags" ./configure --prefix="$BUILD/hfs-install" \
      >"$BUILD/hfs-configure.log" 2>&1 ) \
    || { tail -30 "$BUILD/hfs-configure.log" >&2; die "hfsutils configure failed — log $BUILD/hfs-configure.log"; }

  log "make hfsutil"
  # hfsutil is the single CLI binary all the h* commands dispatch through; the
  # default target would also try the tcl/tk front-ends we have no use for.
  ( cd "$dir" && make hfsutil >"$BUILD/hfs-make.log" 2>&1 ) \
    || { tail -40 "$BUILD/hfs-make.log" >&2; die "hfsutils build failed — log $BUILD/hfs-make.log"; }

  # One binary, dispatched by argv[0] — install a separate COPY per name we use
  # (hard links would share a single code signature slot inside the .app).
  [ -x "$dir/hfsutil" ] || die "hfsutil binary not produced in $dir"
  local t
  for t in "${HFS_TOOLS[@]}"; do install -m 755 "$dir/hfsutil" "$BIN/$t"; done
  log "installed ${HFS_TOOLS[*]} ($(lipo -archs "$BIN/hmount"))"
}

build_unar
build_hfsutils

# The xcodebuild intermediates are ~250 MB and buy nothing once the binaries are
# installed (a rebuild from the kept sources takes ~20s). The SOURCE trees stay:
# they are what makes the LGPL/GPL binaries we ship source-available.
rm -rf "$BUILD"

log "--- extractors ready in $BIN"
for t in "${WANT[@]}"; do
  [ -x "$BIN/$t" ] || die "missing $BIN/$t"
  printf '  %-8s %s\n' "$t" "$(lipo -archs "$BIN/$t")"
done
