#!/usr/bin/env bash
#
# build_deps.sh — fetch and build the CPU-emulator dependency (resource_dasm and
# its support library phosg) into third_party/, at PINNED revisions.
#
# The pins are the point. Both projects move quickly and neither has stable
# releases: an unpinned `git clone` of either one stops compiling against the
# other (or against engine/) within weeks. These two revisions are the pair the
# engine is written and validated against. To move to a newer emulator, change
# the pins here, rebuild, and re-run the module corpus — nowhere else needs to
# know the revisions (CI calls this script and keys its cache on this file).
#
#   usage: tools/build_deps.sh [--force]
#
#   --force   re-fetch and rebuild even if third_party/ is already at the pins
#
set -euo pipefail

# resource_dasm: our perf fork = upstream + the MemoryContext arena-refcount fix
# + the PPC decode cache / basic-block engine the PPC host relies on.
RD_URL="https://github.com/swannman/resource_dasm"
RD_REV="02d8ea9a58eaf559a9194601b33d98723c9d4f60"     # afterdark-perf, 2026-09-05
# phosg: upstream, at the revision that fork was built against.
PH_URL="https://github.com/fuzziqersoftware/phosg"
PH_REV="782785d14d5fe19dda3af71d21d2792e71c5df1d"     # master, 2026-09-04

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TP="$ROOT/third_party"
STAMP="$TP/.deps-stamp"
WANT="phosg=$PH_REV resource_dasm=$RD_REV build=Release"

FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --force) FORCE=1; shift;;
    -h|--help) sed -n '2,16p' "$0"; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

log(){ printf '\033[1;34m[deps]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[deps] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

if [ "$FORCE" = 0 ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$WANT" ] \
   && [ -f "$TP/resource_dasm/build/libresource_file.a" ] && [ -f "$TP/local/lib/libphosg.a" ]; then
  log "already built at the pinned revisions — $TP"
  exit 0
fi

command -v git >/dev/null   || die "need git (xcode-select --install)"
command -v cmake >/dev/null || die "need cmake (brew install cmake)"
clang++ --version >/dev/null 2>&1 \
  || die "clang++ is not usable — install the Command Line Tools (xcode-select --install), or if Xcode is installed accept its licence (sudo xcodebuild -license accept)"

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
mkdir -p "$TP"
rm -f "$STAMP"

# fetch_at <url> <dir> <rev> — check out exactly <rev>, fetching only that commit.
fetch_at(){
  local url="$1" dir="$2" rev="$3"
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$rev" ] \
     && [ -z "$(git -C "$dir" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    log "have $(basename "$dir") @ ${rev:0:7}"; return
  fi
  log "fetching $(basename "$dir") @ ${rev:0:7}"
  rm -rf "$dir"; mkdir -p "$dir"
  git -C "$dir" init --quiet
  git -C "$dir" remote add origin "$url"
  git -C "$dir" fetch --quiet --depth 1 origin "$rev" \
    || die "could not fetch $rev from $url"
  git -C "$dir" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
}

fetch_at "$PH_URL" "$TP/phosg" "$PH_REV"
fetch_at "$RD_URL" "$TP/resource_dasm" "$RD_REV"

# Release is load-bearing: cmake's default build type is empty (-O0), and an
# unoptimized emulator runs every module about 5x slower.
log "building phosg"
rm -rf "$TP/phosg/build" "$TP/local"
( cd "$TP/phosg" \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$TP/local" >"$TP/phosg-build.log" 2>&1 \
  && cmake --build build -j"$JOBS" >>"$TP/phosg-build.log" 2>&1 \
  && cmake --install build >>"$TP/phosg-build.log" 2>&1 ) \
  || { tail -30 "$TP/phosg-build.log" >&2; die "phosg build failed — full log $TP/phosg-build.log"; }

log "building resource_dasm (a minute or so)"
rm -rf "$TP/resource_dasm/build"
( cd "$TP/resource_dasm" \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$TP/local" >"$TP/resource_dasm-build.log" 2>&1 \
  && cmake --build build --target resource_file m68kdasm resource_dasm -j"$JOBS" >>"$TP/resource_dasm-build.log" 2>&1 ) \
  || { tail -30 "$TP/resource_dasm-build.log" >&2; die "resource_dasm build failed — full log $TP/resource_dasm-build.log"; }

echo "$WANT" > "$STAMP"
log "ready: $TP"
