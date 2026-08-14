#!/usr/bin/env bash
#
# adfetch — acquire the original After Dark module trees the emulation hosts read,
# from a source you configure (Internet Archive, a direct StuffIt URL, or a local
# file), and lay them out where the app + hosts expect. Ships NO original bits;
# it only downloads what YOU point it at, under YOUR own responsibility.
#
# It is idempotent and resumable: completed downloads and populated trees are
# skipped; partial downloads resume (curl -C -). Verify with the checksum
# manifest at the end.
#
#   usage: tools/adfetch/adfetch.sh [--assets DIR] [--cache DIR] [--force]
#                                   [--verify-only] [--install-host-libs]
#                                   [--progress] [--cleanup-downloads]
#
#   AD_ASSETS_DIR        destination assets root (default: ~/Library/Application Support/AfterDarkModern/assets)
#   AD_TOOLS_DIR         directory holding the extraction tools (unar + hfsutils'
#                        hmount/hcd/hls/hcopy/humount); prepended to PATH. The .app
#                        sets it to its own bundled Contents/Helpers/bin so a
#                        downloaded app needs nothing installed. Unset = plain PATH.
#   AD_CACHE_DIR         where the big raw downloads land (default: $ASSETS/.adfetch-downloads).
#                        --cache / AD_CACHE_DIR lets the app keep the re-downloadable
#                        ISO/img under ~/Library/Caches instead of Application Support.
#   sources.conf         beside this script — where each source comes from + how to unpack it
#   --install-host-libs  ALSO copy the two shared runtime libraries into engine/
#                         (ADShared40.pef, AD4Library.rsrc), overwriting whatever is there.
#                         The hosts read these from their working directory, so a live dev
#                         setup needs this once; opt in explicitly since it clobbers files
#                         that live outside the assets root and outside git's view (gitignored).
#                         Without it, the same two files are placed under $ASSETS/shared/ only.
#   --progress           (or ADFETCH_PROGRESS=1) emit machine-readable one-line phase
#                         updates on stdout for a GUI to parse:
#                           PROGRESS phase=<name> pct=<0..100> [bytes=<n>]
#                         Human [adfetch] log lines still go to stderr in this mode so the
#                         two streams don't interleave.
#   --cleanup-downloads   (or ADFETCH_CLEANUP=1) delete the raw download cache after a
#                         SUCCESSFUL verify (the ISO/img are re-downloadable, not precious).
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# ../adhost only exists in a dev checkout; a bundled copy of this tool won't have
# it. It's only read for --install-host-libs, so resolve it leniently.
ADHOST_DIR="$(cd "$HERE/../adhost" 2>/dev/null && pwd || echo "$HERE/../adhost")"
CONF="$HERE/sources.conf"

ASSETS="${AD_ASSETS_DIR:-$HOME/Library/Application Support/AfterDarkModern/assets}"
CACHE="${AD_CACHE_DIR:-}"
FORCE=0; VERIFY_ONLY=0; INSTALL_HOST_LIBS=0
PROGRESS="${ADFETCH_PROGRESS:-0}"; CLEANUP="${ADFETCH_CLEANUP:-0}"
while [ $# -gt 0 ]; do
  case "$1" in
    --assets) ASSETS="$2"; shift 2;;
    --cache) CACHE="$2"; shift 2;;
    --force) FORCE=1; shift;;
    --verify-only) VERIFY_ONLY=1; shift;;
    --install-host-libs) INSTALL_HOST_LIBS=1; shift;;
    --progress) PROGRESS=1; shift;;
    --cleanup-downloads) CLEANUP=1; shift;;
    -h|--help) sed -n '2,26p' "$0"; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

# In --progress mode, keep the human log on stderr so the PROGRESS lines are the
# only thing on stdout (the GUI parses stdout line by line).
_logfd=1; [ "$PROGRESS" = 1 ] && _logfd=2
log(){ printf '\033[1;36m[adfetch]\033[0m %s\n' "$*" >&$_logfd; }
warn(){ printf '\033[1;33m[adfetch] WARN:\033[0m %s\n' "$*" >&2; }
die(){ printf '\033[1;31m[adfetch] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# progress <phase> <pct>            — a phase transition (pct is the overall %).
# progress_bytes <phase> <pct> <n>  — a byte tick during a download.
# CUR_PHASE / CUR_PCT let download() emit byte ticks without extra plumbing.
CUR_PHASE=""; CUR_PCT=0
progress(){ CUR_PHASE="$1"; CUR_PCT="$2"; [ "$PROGRESS" = 1 ] && printf 'PROGRESS phase=%s pct=%s\n' "$1" "$2" || true; }
progress_bytes(){ [ "$PROGRESS" = 1 ] && printf 'PROGRESS phase=%s pct=%s bytes=%s\n' "$1" "$2" "$3" || true; }

# A bundled copy of the extraction tools (AfterDark.app/Contents/Helpers/bin)
# wins over anything installed, so the app's behaviour doesn't depend on what
# happens to be on the user's machine. Dev/CLI runs leave AD_TOOLS_DIR unset and
# keep using PATH (Homebrew).
if [ -n "${AD_TOOLS_DIR:-}" ] && [ -d "$AD_TOOLS_DIR" ]; then
  PATH="$AD_TOOLS_DIR:$PATH"; export PATH
fi

command -v unar >/dev/null || die "need 'unar' (brew install unar)"
command -v curl >/dev/null || die "need 'curl'"
[ -f "$CONF" ] || die "missing $CONF"

# hfsutils (hmount/hcd/hls/hcopy/humount) is only needed for the hfsimg2/hfsiso
# modes (reading Internet Archive Mac disk images); checked lazily in unpack_hfs
# so a direct-StuffIt run doesn't require it.
need_hfsutils(){
  local t
  for t in hmount hcd hls hcopy humount; do
    command -v "$t" >/dev/null \
      || die "need hfsutils ($t) to read Internet Archive HFS disk images — brew install hfsutils"
  done
}
# shellcheck disable=SC1090
source "$CONF"
# manifest_rows / md5of / zero_rsrc_pad — a shell-only manifest reader, so this
# script (which ships inside the .app) needs no python3.
# shellcheck source=manifest.sh
source "$HERE/manifest.sh"
MANIFEST="$HERE/assets.manifest.json"
[ -f "$MANIFEST" ] || die "missing $MANIFEST"

DL="${CACHE:-$ASSETS/.adfetch-downloads}"
mkdir -p "$ASSETS" "$DL"

# md5 of the two reference StuffIt archives, straight from the manifest.
ref_archive_md5(){
  manifest_rows "$MANIFEST" sourceArchives path md5 \
    | awk -F"$MANIFEST_FS" -v want="$1" 'index($1, want) && index($1, want) == length($1) - length(want) + 1 {print $2}'
}
AD9_REF_MD5="$(ref_archive_md5 AD9v11fullpackage.sit)"
DLX_REF_MD5="$(ref_archive_md5 After_Dark_Deluxe_4.1.sit)"

# download <url> <dest>  — resumable; supports file:// (copy) and http(s). In
# --progress mode an http download runs a background size-poller that emits
# `PROGRESS phase=<CUR_PHASE> pct=<CUR_PCT> bytes=<n>` ~1/s so the GUI can show a
# live byte count (the total isn't known up front, so pct stays the phase base).
download(){
  local url="$1" dest="$2"
  if [ -f "$dest" ] && [ "$FORCE" = 0 ]; then log "have $(basename "$dest") (skip download)"; return; fi
  case "$url" in
    file://*) log "copy $(basename "$dest") from local file"; cp -f "${url#file://}" "$dest";;
    http://*|https://*)
      log "download $(basename "$dest")"
      local poller=""
      if [ "$PROGRESS" = 1 ]; then
        local ph="$CUR_PHASE" pc="$CUR_PCT"
        ( while :; do
            sleep 1
            [ -f "$dest" ] && progress_bytes "$ph" "$pc" "$(filesize "$dest")"
          done ) & poller=$!
      fi
      curl -fL -C - --retry 3 -o "$dest" "$url" \
        && { [ -n "$poller" ] && kill "$poller" 2>/dev/null; true; } \
        || { [ -n "$poller" ] && kill "$poller" 2>/dev/null; false; }
      [ -n "$poller" ] && wait "$poller" 2>/dev/null || true
      ;;
    "") die "empty URL — edit tools/adfetch/sources.conf";;
    *) die "unsupported URL scheme: $url";;
  esac
}

# unpack <archive> <mode> <staging_dir>  — leaves the extracted tree in staging.
# unar exits non-zero when a member fails to expand (these archives contain an
# empty placeholder dir that always "fails"); that is benign, so we don't abort
# on unar's status — find_tree validates the real tree afterward.
unpack(){
  local arc="$1" mode="$2" stage="$3"
  rm -rf "$stage"; mkdir -p "$stage"
  case "$mode" in
    sit1) unar -quiet -force-overwrite -output-directory "$stage" "$arc" >/dev/null || true;;
    sit2)
      local mid="$stage/.mid"; mkdir -p "$mid"
      unar -quiet -force-overwrite -no-directory -output-directory "$mid" "$arc" >/dev/null || true
      local inner; inner="$(find "$mid" -maxdepth 1 -type f | head -1)"
      [ -n "$inner" ] || die "sit2: no inner archive found in $arc"
      unar -quiet -force-overwrite -output-directory "$stage" "$inner" >/dev/null || true
      rm -rf "$mid";;
    *) die "unknown MODE '$mode'";;
  esac
}

# find_tree <staging> <needle>  — echo the dir under staging whose path contains
# needle (the AD tree may sit one level down inside a mounted image).
find_tree(){
  local stage="$1" needle="$2" hit
  hit="$(find "$stage" -maxdepth 4 -type d -name "$needle" 2>/dev/null | head -1)"
  echo "$hit"
}

# unpack_hfs <arc> <mode> <pool>  — mounts an HFS/HFS+ disk image or ISO with
# hfsutils and materializes a FLAT pool of native (data+resource fork) files
# into <pool>. Internet Archive images don't lay out the folder tree adfetch
# expects, so extraction here is flat; place_pool() matches results by filename.
unpack_hfs(){
  local arc="$1" mode="$2" pool="$3"
  need_hfsutils
  rm -rf "$pool"; mkdir -p "$pool"
  humount >/dev/null 2>&1 || true
  hmount "$arc" >/dev/null || die "hmount failed on $arc"
  case "$mode" in
    hfsimg2)
      # AD9 IA image: a DECOY empty "After Dark 4.0" folder sits at the image's
      # top level; the real tree is nested. Per-file hcopy -m + unar loop.
      hcd "System Folder" && hcd "Control Panels" && hcd "After Dark Files" && hcd "After Dark 4.0" \
        || { humount >/dev/null 2>&1; die "hfsimg2: expected After Dark 4.0 folder not found in $arc"; }
      hls -1 | while IFS= read -r name; do
        [ -z "$name" ] && continue
        hcopy -m "$name" "$pool/.item.bin" 2>>"$pool.log"
        unar -quiet -force-overwrite -no-directory -output-directory "$pool" "$pool/.item.bin" >>"$pool.log" 2>&1
        rm -f "$pool/.item.bin"
      done
      # the AD4 library lives one level up, in the sibling "Module Resources" folder.
      hcd "::" && hcd "Module Resources"
      hcopy -m "After Dark 4.0 Library" "$pool/.item.bin" 2>>"$pool.log"
      unar -quiet -force-overwrite -no-directory -output-directory "$pool" "$pool/.item.bin" >>"$pool.log" 2>&1
      rm -f "$pool/.item.bin"
      ;;
    hfsiso)
      # Deluxe CD: the real tree is packed as a StuffIt archive INSIDE the
      # top-level "Double-Click Me to Install" installer (a MacBinary-wrapped
      # SEA whose payload is itself a nested StuffIt archive) — two unar passes,
      # result lands flat.
      hcopy -m "Double-Click Me to Install" "$pool/installer.bin" 2>>"$pool.log"
      local mid="$pool/.mid"; mkdir -p "$mid"
      unar -quiet -force-overwrite -output-directory "$mid" "$pool/installer.bin" >>"$pool.log" 2>&1 || true
      unar -quiet -force-overwrite -no-directory -output-directory "$pool" "$mid/Double-Click Me to Install" >>"$pool.log" 2>&1 || true
      rm -rf "$mid" "$pool/installer.bin"
      ;;
    *) humount >/dev/null 2>&1; die "unknown hfs MODE '$mode'";;
  esac
  humount >/dev/null 2>&1 || true
}

# place_pool <pool> <family> <dest_root> [shared_label_substr]  — copy every
# module fork the manifest lists for <family> (plus the shared-library entry
# whose label contains [shared_label_substr], if given) out of a FLAT
# extraction pool into the tree layout the hosts expect, matched by filename.
place_pool(){
  local pool="$1" family="$2" dest_root="$3" shared_substr="${4:-}"
  local ok=0 missing=0 name rel
  while IFS="$MANIFEST_FS" read -r name rel; do
    [ -n "$name" ] || continue
    local src="$pool/$name" dest="$dest_root/$rel"
    if [ ! -e "$src" ]; then missing=$((missing + 1)); warn "IA pool: missing '$name'"; continue; fi
    mkdir -p "$(dirname "$dest")"
    cp -p "$src" "$dest"
    ok=$((ok + 1))
  done < <(pool_wants "$family" "$shared_substr")
  log "placed $ok from IA pool ($missing missing) — $family"
}

# pool_wants <family> [shared_label_substr]  — emit "<pool filename><FS><relative
# dest path>" for every fork the manifest lists for that family (plus the
# matching shared library). A fork's dest is its file path with the
# /..namedfork/rsrc suffix dropped: the flat pool holds whole files (both forks),
# and cp -p carries the resource fork along.
pool_wants(){
  local family="$1" shared_substr="${2:-}" p rel
  {
    manifest_rows "$MANIFEST" moduleAssets family path \
      | awk -F"$MANIFEST_FS" -v fam="$family" '$1 == fam {print $2}'
    if [ -n "$shared_substr" ]; then
      manifest_rows "$MANIFEST" sharedLibraries label path \
        | awk -F"$MANIFEST_FS" -v want="$shared_substr" 'index($1, want) {print $2}'
    fi
  } | while IFS= read -r p; do
    [ -n "$p" ] || continue
    rel="${p%/..namedfork/rsrc}"
    printf '%s%s%s\n' "${rel##*/}" "$MANIFEST_FS" "$rel"
  done
}

# zero_rsrc_header <fork-path>  — zero resource-fork bytes 16-255 (the fixed
# 240-byte "reserved for system use" pad before resource DATA begins at byte
# 256, Inside Macintosh). That region is scratch space no Mac tool ever reads;
# different distributions of the same content leave different leftover
# extraction-tool buffer bytes there. Canonicalizing it here means adfetch's
# output is deterministic regardless of source (reference StuffIt or the IA
# disk image) -- see assets.manifest.json's 'note' and sources.conf for the
# investigation that established this. (zero_rsrc_pad lives in manifest.sh,
# shared with the verifier's md5Canonical check.)
zero_rsrc_header(){ zero_rsrc_pad "$1"; }

# canonicalize_ad9_tree <After Dark 4.0 folder>  — canonicalize the resource
# fork of every item placed there (module files + the shared-lib item, if
# present). Safe to run on any tree, from any source.
canonicalize_ad9_tree(){
  local dir="$1" name
  [ -d "$dir" ] || return 0
  find "$dir" -maxdepth 1 -type f | while IFS= read -r f; do
    zero_rsrc_header "$f/..namedfork/rsrc"
  done
}

acquire_ad9(){
  local url="${AD9_URL:-}" mode="${AD9_MODE:-sit1}"
  if [ -z "$url" ] || [ "${ADFETCH_USE_IA:-0}" = 1 ]; then
    url="${AD9_URL_IA:-}"; mode="${AD9_MODE_IA:-hfsimg2}"
    log "using Internet Archive AD9 fallback ($mode) — content-identical to the reference"
    log "build (per-resource verified); raw fork md5s differ only in unused header padding."
  fi
  local dest="$DL/ad9-source"
  local target="$ASSETS/extracted/After Dark 9 v1.0 (9:9:03)"
  if [ -d "$target" ] && [ "$FORCE" = 0 ]; then log "AD9 tree present (skip)"; return; fi
  progress ad9-download 5
  download "$url" "$dest"
  progress ad9-extract 15
  if [ "$mode" = sit1 ]; then
    local m; m="$(md5of "$dest")"
    [ "$m" = "$AD9_REF_MD5" ] && log "AD9 archive md5 MATCHES reference (exact build)" \
                              || warn "AD9 archive md5 $m != reference $AD9_REF_MD5 (different build)"
  fi
  log "unpacking AD9 ($mode)"
  if [ "$mode" = hfsimg2 ]; then
    local pool="$DL/ad9-pool"; unpack_hfs "$dest" "$mode" "$pool"
    place_pool "$pool" PPC "$ASSETS" Library
    rm -rf "$pool" "$pool.log"
    log "canonicalizing PPC resource forks (zeroing header pad)"
    canonicalize_ad9_tree "$target/After Dark Files/After Dark 4.0"
    zero_rsrc_header "$target/After Dark Files/Module Resources/After Dark 4.0 Library/..namedfork/rsrc"
    return
  fi
  local stage="$DL/ad9-stage"; unpack "$dest" "$mode" "$stage"
  local tree; tree="$(find_tree "$stage" "After Dark 9 v1.0 (9:9:03)")"
  [ -n "$tree" ] || die "AD9: could not locate 'After Dark 9 v1.0 (9:9:03)' in extracted image"
  mkdir -p "$ASSETS/extracted"; rm -rf "$target"; mv "$tree" "$target"
  rm -rf "$stage"
  log "canonicalizing PPC resource forks (zeroing header pad)"
  canonicalize_ad9_tree "$target/After Dark Files/After Dark 4.0"
  zero_rsrc_header "$target/After Dark Files/Module Resources/After Dark 4.0 Library/..namedfork/rsrc"
}

acquire_dlx(){
  local url="${DLX_URL:-}" mode="${DLX_MODE:-sit2}"
  if [ -z "$url" ] || [ "${ADFETCH_USE_IA:-0}" = 1 ]; then
    url="${DLX_URL_IA:-}"; mode="${DLX_MODE_IA:-hfsiso}"
    warn "using Internet Archive Deluxe fallback ($mode) — build may differ from manifest"
  fi
  local dest="$DL/dlx-source"
  local target="$ASSETS/deluxe/extracted/dlx/After Dark Deluxe (4"
  if [ -d "$target" ] && [ "$FORCE" = 0 ]; then log "Deluxe tree present (skip)"; return; fi
  progress dlx-download 20
  download "$url" "$dest"
  if [ "$mode" = sit2 ]; then
    local m; m="$(md5of "$dest")"
    [ "$m" = "$DLX_REF_MD5" ] && log "Deluxe archive md5 MATCHES reference (exact build)" \
                             || warn "Deluxe archive md5 $m != reference $DLX_REF_MD5 (different build)"
  fi
  progress dlx-extract 72
  log "unpacking Deluxe ($mode)"
  if [ "$mode" = hfsiso ]; then
    local pool="$DL/dlx-pool"; unpack_hfs "$dest" "$mode" "$pool"
    mkdir -p "$target"
    place_pool "$pool" 68K "$ASSETS" Shared
    rm -rf "$pool" "$pool.log"
    return
  fi
  local stage="$DL/dlx-stage"; unpack "$dest" "$mode" "$stage"
  # sit2 produces ".../After Dark Deluxe (4"; image modes need a search.
  local tree; tree="$(find_tree "$stage" "After Dark Deluxe (4")"
  [ -n "$tree" ] || die "Deluxe: could not locate 'After Dark Deluxe (4' tree in extracted source"
  mkdir -p "$ASSETS/deluxe/extracted/dlx"; rm -rf "$target"; mv "$tree" "$target"
  rm -rf "$stage"
}

# Copy the two shared libraries out of the extracted trees, canonical copy
# under the assets root ($ASSETS/shared/). The RUNNING hosts read these two
# files from their working directory (engine/), not from the assets
# root — but writing there unconditionally used to silently overwrite a
# developer's live engine/{ADShared40.pef,AD4Library.rsrc} on every run
# (those files are gitignored, so `git status` never showed the clobber). Now
# that only happens with an explicit --install-host-libs opt-in, which prints
# what it overwrites.
SHARED_DIR="$ASSETS/shared"
place_shared_libs(){
  local shared_data="$ASSETS/deluxe/extracted/dlx/After Dark Deluxe (4/After Dark 4.0 Shared"
  local lib_rsrc="$ASSETS/extracted/After Dark 9 v1.0 (9:9:03)/After Dark Files/Module Resources/After Dark 4.0 Library/..namedfork/rsrc"
  mkdir -p "$SHARED_DIR"
  if [ -f "$shared_data" ]; then
    cp -f "$shared_data" "$SHARED_DIR/ADShared40.pef"; log "placed ADShared40.pef under assets root ($SHARED_DIR)"
  else warn "missing 'After Dark 4.0 Shared' — ADShared40.pef not placed"; fi
  if [ -f "$lib_rsrc" ]; then
    cp -f "$lib_rsrc" "$SHARED_DIR/AD4Library.rsrc"
    zero_rsrc_header "$SHARED_DIR/AD4Library.rsrc"
    log "placed AD4Library.rsrc under assets root ($SHARED_DIR)"
  else warn "missing 'After Dark 4.0 Library' rsrc — AD4Library.rsrc not placed"; fi

  if [ "$INSTALL_HOST_LIBS" = 1 ]; then
    log "--install-host-libs: installing shared libs into $ADHOST_DIR (dev workflow)"
    local f
    for f in ADShared40.pef AD4Library.rsrc; do
      [ -f "$SHARED_DIR/$f" ] || continue
      if [ -f "$ADHOST_DIR/$f" ]; then
        warn "overwriting $ADHOST_DIR/$f (was $(md5of "$ADHOST_DIR/$f")) with $(md5of "$SHARED_DIR/$f")"
      fi
      cp -f "$SHARED_DIR/$f" "$ADHOST_DIR/$f"
      log "installed $ADHOST_DIR/$f"
    done
  fi
}

if [ "$VERIFY_ONLY" = 0 ]; then
  log "assets root: $ASSETS"
  acquire_ad9
  acquire_dlx
  progress shared 82
  place_shared_libs
fi

# Verify the shared libs wherever this run actually put them: engine/
# only with --install-host-libs, otherwise the assets-root copy.
VERIFY_ADHOST_DIR="$ADHOST_DIR"
[ "$INSTALL_HOST_LIBS" = 1 ] || VERIFY_ADHOST_DIR="$SHARED_DIR"

progress verify 88
log "verifying against manifest ..."
# In --progress mode keep stdout clean (PROGRESS lines only) by sending the
# verifier's own report to the log stream (stderr) and quieting the per-file list.
VERIFY_QUIET=""; [ "$PROGRESS" = 1 ] && VERIFY_QUIET="--quiet"
if bash "$HERE/verify_manifest.sh" "$ASSETS" --adhost-dir "$VERIFY_ADHOST_DIR" $VERIFY_QUIET 1>&$_logfd; then
  log "DONE — all needed forks present and byte-identical to the reference build."
  # Drop the big re-downloadable raw sources once the tree is verified good.
  if [ "$CLEANUP" = 1 ] && [ "$VERIFY_ONLY" = 0 ]; then
    log "cleaning up raw downloads in $DL"
    rm -rf "$DL"
  fi
  progress done 100
else
  warn "some files missing or different from the reference build (see above)."
  warn "If you used an Internet Archive image, that is expected (different distribution)."
  warn "The emulation may still work; supply the exact StuffIt in sources.conf for a guaranteed match."
  progress error 88
  exit 1
fi
