#!/usr/bin/env bash
#
# Verify a populated assets root against tools/adfetch/assets.manifest.json.
#
# Checks the md5 of every needed fork (module resource/data forks + the two
# shared libraries) at its expected path. Exit 0 iff everything the hosts read
# is present and byte-identical (or, for PPC resource forks, content-identical
# -- see below) to the reference build the manifest was generated from.
#
# PPC resource forks (module rsrc entries + the AD4 Library) also carry a
# 'md5Canonical' value in the manifest: the md5 after zeroing resource-fork
# bytes 16-255, the fixed 240-byte "reserved for system use" pad before resource
# DATA begins at byte 256 (Inside Macintosh). That region is scratch space no
# Mac tool ever reads; different distributions (e.g. our reference
# AD9v11fullpackage.sit vs. the Internet Archive AfterDark40.img fallback) leave
# different leftover buffer bytes there even when the actual resource content is
# byte-identical. A file matches if EITHER its raw md5 equals 'md5' (exact
# reference build) OR its canonicalized md5 equals 'md5Canonical'
# (content-identical build, header junk aside).
#
# This is the shell twin of verify_manifest.py -- same manifest, same rules,
# same output -- and it is the one the app runs, because a downloaded .app
# cannot assume python3 is installed. (The .py is kept for dev use.)
#
# usage: verify_manifest.sh <assets_root> [--adhost-dir DIR] [--quiet]
#   <assets_root>      root the module forks live under (== AD_ASSETS_DIR)
#   --adhost-dir DIR   where the shared libs (ADShared40.pef / AD4Library.rsrc)
#                      are expected (default: ../adhost relative to this script)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="$HERE/assets.manifest.json"
# shellcheck source=manifest.sh
source "$HERE/manifest.sh"

ADHOST_DIR="$HERE/../adhost"
QUIET=0
ASSETS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --adhost-dir) ADHOST_DIR="$2"; shift 2;;
    --quiet) QUIET=1; shift;;
    -h|--help) sed -n '2,30p' "$0"; exit 0;;
    *) ASSETS="$1"; shift;;
  esac
done
[ -n "$ASSETS" ] || { echo "usage: verify_manifest.sh <assets_root> [--adhost-dir DIR]" >&2; exit 2; }
[ -f "$MANIFEST" ] || { echo "missing $MANIFEST" >&2; exit 2; }

OK=0; MISS=0; DIFF=0

# check <path> <want_md5> <label> [want_canonical_md5]
check(){
  local path="$1" want="$2" label="$3" want_canon="${4:-}" got
  if [ ! -f "$path" ]; then
    MISS=$((MISS + 1)); printf 'MISSING %s: %s\n' "$label" "$path"; return
  fi
  got="$(md5of "$path")"
  if [ "$got" = "$want" ]; then
    OK=$((OK + 1)); [ "$QUIET" = 1 ] || printf 'ok      %s\n' "$label"
  elif [ -n "$want_canon" ] && [ "$(md5_canonical "$path")" = "$want_canon" ]; then
    OK=$((OK + 1))
    [ "$QUIET" = 1 ] || printf 'ok      %s (content-identical, different build -- see md5Canonical)\n' "$label"
  else
    DIFF=$((DIFF + 1))
    printf 'DIFFER  %s: %s\n        want %s got %s\n' "$label" "$path" "$want" "$got"
  fi
}

# Shared libraries land next to the hosts (ADHOST_DIR), fetched from the two
# archives. Their manifest 'path' is the SOURCE fork; we verify the copy.
while IFS="$MANIFEST_FS" read -r label md5 canon; do
  [ -n "$label" ] || continue
  case "$label" in
    *Shared*) dest=ADShared40.pef;;
    *)        dest=AD4Library.rsrc;;
  esac
  check "$ADHOST_DIR/$dest" "$md5" "sharedlib $dest" "$canon"
done < <(manifest_rows "$MANIFEST" sharedLibraries label md5 md5Canonical)

while IFS="$MANIFEST_FS" read -r path md5 canon family module kind; do
  [ -n "$path" ] || continue
  check "$ASSETS/$path" "$md5" "$family $module ($kind)" "$canon"
done < <(manifest_rows "$MANIFEST" moduleAssets path md5 md5Canonical family module kind)

TOTAL=$((OK + MISS + DIFF))
printf '\n=== manifest verify: %d/%d ok, %d differ, %d missing ===\n' "$OK" "$TOTAL" "$DIFF" "$MISS"
[ "$MISS" = 0 ] && [ "$DIFF" = 0 ]
