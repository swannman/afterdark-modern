#!/usr/bin/env bash
#
# manifest.sh — the small shell helpers adfetch.sh and verify_manifest.sh share:
# reading assets.manifest.json and hashing a file. Sourced, never run.
#
# Why not python3/jq: this ships INSIDE AfterDark.app, which must run on a Mac
# with nothing installed beyond macOS itself. awk, dd, and one of md5/openssl
# are all base-system tools.

# Field separator for manifest_rows output. A TAB would be wrong: bash's `read`
# treats IFS whitespace specially and would COLLAPSE the empty field an absent
# key produces. US (0x1f) is not whitespace, so empty fields survive, and no
# manifest value can contain it.
MANIFEST_FS=$'\037'

# manifest_rows <manifest.json> <array> <field...>
#   Emit one row per object in the named top-level array, fields in the
#   requested order separated by $MANIFEST_FS (empty when a field is absent).
#   Read them with:  while IFS="$MANIFEST_FS" read -r a b c; do … done
#
#   The manifest is machine-written and pretty-printed one field per line at a
#   fixed indent, which is what makes this line-oriented read exact:
#     " \"moduleAssets\": ["   array,  "  {" / "  }" record,  "   \"k\": v" field.
#   LC_ALL=C so sprintf("%c") in the \uXXXX decoder emits single BYTES.
manifest_rows(){
  local file="$1" array="$2"; shift 2
  LC_ALL=C awk -v want="$array" -v fields="$*" -v FS_OUT="$MANIFEST_FS" '
    function hexval(h,   i, v, d) {
      v = 0
      for (i = 1; i <= length(h); i++) {
        d = index("0123456789abcdef", tolower(substr(h, i, 1))) - 1
        v = v * 16 + d
      }
      return v
    }
    # UTF-8 encode a code point as raw bytes (BMP is all the manifest uses).
    function utf8(cp) {
      if (cp < 128)  return sprintf("%c", cp)
      if (cp < 2048) return sprintf("%c%c", 192 + int(cp / 64), 128 + cp % 64)
      return sprintf("%c%c%c", 224 + int(cp / 4096), 128 + int(cp / 64) % 64, 128 + cp % 64)
    }
    function unescape(s,   out, i, c) {
      out = ""
      while ((i = index(s, "\\")) > 0) {
        out = out substr(s, 1, i - 1)
        c = substr(s, i + 1, 1)
        if (c == "u")      { out = out utf8(hexval(substr(s, i + 2, 4))); s = substr(s, i + 6) }
        else if (c == "n") { out = out "\n"; s = substr(s, i + 2) }
        else if (c == "t") { out = out "\t"; s = substr(s, i + 2) }
        else               { out = out c;    s = substr(s, i + 2) }   # \\ \" \/
      }
      return out s
    }
    BEGIN { nf = split(fields, F, " ") }
    /^ "[A-Za-z]+": \[/ { sec = $0; sub(/^ "/, "", sec); sub(/".*/, "", sec); next }
    sec != want { next }
    /^  \{/ { split("", V); next }
    /^  \}/ {
      row = ""
      for (i = 1; i <= nf; i++) row = row (i > 1 ? FS_OUT : "") V[F[i]]
      print row
      next
    }
    /^   "/ {
      key = $0; sub(/^   "/, "", key); sub(/":.*/, "", key)
      val = $0; sub(/^   "[^"]*": /, "", val); sub(/,$/, "", val)
      if (val ~ /^".*"$/) val = unescape(substr(val, 2, length(val) - 2))
      V[key] = val
    }
  ' "$file"
}

# NOTE on `[ -f ]` vs `[ -r ]` below: access(2) reports a resource fork
# (…/..namedfork/rsrc) as unreadable even when it opens fine, so `test -r`
# false-negatives on every fork in this manifest. `test -f` answers correctly.

# md5of <path> — md5 hex of a file, or empty if it isn't there. Tries the macOS
# `md5` (also at its absolute path, since a GUI-launched app's PATH may not
# carry /sbin), then openssl, then GNU md5sum.
md5of(){
  [ -f "$1" ] || return 0
  if command -v md5 >/dev/null 2>&1; then md5 -q "$1"
  elif [ -x /sbin/md5 ]; then /sbin/md5 -q "$1"
  elif command -v openssl >/dev/null 2>&1; then openssl md5 -r "$1" | awk '{print $1}'
  elif command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'
  else echo "no md5 tool (md5/openssl/md5sum) found" >&2; return 1
  fi
}

# filesize <path> — portable byte size (BSD stat on macOS, GNU stat elsewhere).
filesize(){ stat -f%z "$1" 2>/dev/null || stat -c%s "$1" 2>/dev/null || echo 0; }

# zero_rsrc_pad <path> — zero resource-fork bytes 16-255: the fixed 240-byte
# "reserved for system use" pad before resource DATA begins at byte 256 (Inside
# Macintosh). Scratch space no Mac tool reads, but extraction tools leave
# different leftovers there, so canonicalizing it makes adfetch's output
# source-independent. In place, never changes the file's length.
zero_rsrc_pad(){
  local f="$1" sz n
  [ -f "$f" ] || return 0
  sz="$(filesize "$f")"
  [ "$sz" -gt 16 ] || return 0
  n=$(( sz < 256 ? sz - 16 : 240 ))
  dd if=/dev/zero of="$f" bs=1 seek=16 count="$n" conv=notrunc 2>/dev/null
}

# md5_canonical <path> — the md5 the file WOULD have with that pad zeroed,
# without touching the original (see verify_manifest's md5Canonical).
md5_canonical(){
  local f="$1" tmp out
  [ -f "$f" ] || return 0
  tmp="$(mktemp -t adfetch-canon)" || return 1
  cat "$f" > "$tmp"
  zero_rsrc_pad "$tmp"
  out="$(md5of "$tmp")"
  rm -f "$tmp"
  printf '%s\n' "$out"
}
