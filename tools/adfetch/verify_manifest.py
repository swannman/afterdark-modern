#!/usr/bin/env python3
"""Verify a populated assets root against tools/adfetch/assets.manifest.json.

Checks the md5 of every needed fork (module resource/data forks + the two shared
libraries) at its expected path. Exit 0 iff everything the hosts read is present
and byte-identical (or, for PPC resource forks, content-identical -- see below)
to the reference build the manifest was generated from.

PPC resource forks (module rsrc entries + the AD4 Library) also carry a
'md5Canonical' value in the manifest: the md5 after zeroing resource-fork bytes
16-255, the fixed 240-byte "reserved for system use" pad before resource DATA
begins at byte 256 (Inside Macintosh). That region is scratch space no Mac tool
ever reads; different distributions (e.g. our reference AD9v11fullpackage.sit
vs. the Internet Archive AfterDark40.img fallback) leave different leftover
buffer bytes there even when the actual resource content is byte-identical. A
file matches if EITHER its raw md5 equals 'md5' (exact reference build) OR its
canonicalized md5 equals 'md5Canonical' (content-identical build, header junk
aside) -- confirmed by direct resource_dasm decode diff (0 bytes differ) for
Time Flies, Flying Toasters!, Swirling Magic, Rodger Dodger, Art Critic,
Marbles!, and the AD4 Library.

usage: verify_manifest.py <assets_root> [--adhost-dir DIR] [--quiet]
  <assets_root>      root the module forks live under (== AD_ASSETS_DIR)
  --adhost-dir DIR   where the shared libs (ADShared40.pef / AD4Library.rsrc)
                     are expected (default: ../adhost relative to this script)
"""
import hashlib, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "assets.manifest.json")

def md5(path):
    try:
        h = hashlib.md5()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None

def md5_canonical(path):
    """md5 of a resource fork with bytes 16-255 (the reserved header pad)
    zeroed -- see module docstring."""
    try:
        with open(path, "rb") as f:
            data = bytearray(f.read())
    except OSError:
        return None
    for i in range(16, min(256, len(data))):
        data[i] = 0
    return hashlib.md5(bytes(data)).hexdigest()

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quiet = "--quiet" in sys.argv
    adhost_dir = os.path.normpath(os.path.join(HERE, "..", "adhost"))
    if "--adhost-dir" in sys.argv:
        adhost_dir = sys.argv[sys.argv.index("--adhost-dir") + 1]
    if not args:
        print("usage: verify_manifest.py <assets_root> [--adhost-dir DIR]", file=sys.stderr)
        return 2
    assets_root = args[0]
    man = json.load(open(MANIFEST, encoding="utf-8"))

    ok = miss = diff = 0
    def check(path, want, label, want_canonical=None):
        nonlocal ok, miss, diff
        got = md5(path)
        if got is None:
            miss += 1
            print(f"MISSING {label}: {path}")
        elif got == want:
            ok += 1
            if not quiet:
                print(f"ok      {label}")
        elif want_canonical and md5_canonical(path) == want_canonical:
            ok += 1
            if not quiet:
                print(f"ok      {label} (content-identical, different build -- see md5Canonical)")
        else:
            diff += 1
            print(f"DIFFER  {label}: {path}\n        want {want} got {got}")

    # Shared libraries land next to the hosts (adhost_dir), fetched from the two
    # archives. Their manifest 'path' is the SOURCE fork; we verify the copy.
    shared_dest = {"ADShared40.pef": None, "AD4Library.rsrc": None}
    for s in man["sharedLibraries"]:
        dest = "ADShared40.pef" if "Shared" in s["label"] else "AD4Library.rsrc"
        check(os.path.join(adhost_dir, dest), s["md5"], f"sharedlib {dest}", s.get("md5Canonical"))

    for e in man["moduleAssets"]:
        check(os.path.join(assets_root, e["path"]), e["md5"],
              f"{e['family']} {e['module']} ({e['kind']})", e.get("md5Canonical"))

    total = ok + miss + diff
    print(f"\n=== manifest verify: {ok}/{total} ok, {diff} differ, {miss} missing ===")
    return 0 if (miss == 0 and diff == 0) else 1

if __name__ == "__main__":
    sys.exit(main())
