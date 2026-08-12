# adfetch — original-asset acquisition

The emulation hosts run the **original** After Dark module code; this repository
ships **none** of it. `adfetch` downloads the originals from a source you
configure and lays them out where the app and hosts expect. You are responsible
for your own right to download and use those files — the originals are
copyrighted by Berkeley Systems (the After Dark brand continues via Infinisys).

## What it produces

Into `$AD_ASSETS_DIR` (default `~/Library/Application Support/AfterDarkModern/assets`):

```
extracted/After Dark 9 v1.0 (9:9:03)/…                    # PPC AD 4.0 modules
deluxe/extracted/dlx/After Dark Deluxe (4/…               # 68K Deluxe modules
```

and copies the two shared runtime libraries next to each other under the assets root:

```
$AD_ASSETS_DIR/shared/ADShared40.pef      # data fork of "After Dark 4.0 Shared"
$AD_ASSETS_DIR/shared/AD4Library.rsrc     # rsrc fork of "After Dark 4.0 Library"
```

`catalog.json` paths are **relative** to these roots (resolved at runtime by
`ADPaths`), so once the assets are in place the app finds every module.

### Dev workflow: `--install-host-libs`

The **running hosts** (`tools/adhost/adhost` / `adhost68k`) read the two shared
libraries from their own working directory, not from the assets root — so a
live dev checkout needs copies at `tools/adhost/ADShared40.pef` and
`tools/adhost/AD4Library.rsrc`. `adfetch.sh` does **not** write there by
default: those two files are gitignored (so `git status` won't show a
clobber), and a plain run used to silently overwrite a developer's existing
copies on every invocation. Pass `--install-host-libs` to opt in — it copies
the assets-root shared libs into `tools/adhost/`, printing the md5 of
whatever it overwrites:

```bash
tools/adfetch/adfetch.sh --install-host-libs
```

Dev setups only need this once (or again after `--force` re-extracts from a
different source); it's harmless to repeat.

## Use

1. `brew install unar` (StuffIt / disk-image extraction that preserves resource forks).
   If you'll use the Internet Archive fallbacks, also `brew install hfsutils`
   (adfetch checks for this and tells you if it's missing).
2. Edit `sources.conf` — point `AD9_URL` / `DLX_URL` at your source (a direct
   StuffIt URL, a `file:///…/local.sit`, or leave blank to use the documented
   Internet Archive disk-image fallbacks).
3. Run it:

   ```bash
   AD_ASSETS_DIR="$HOME/Library/Application Support/AfterDarkModern/assets" \
     tools/adfetch/adfetch.sh --install-host-libs
   ```

   (drop `--install-host-libs` if you only want the assets root populated,
   e.g. for CI or a from-scratch verification run.)

It is **idempotent and resumable**: finished downloads and populated trees are
skipped; partial downloads resume. `--force` re-extracts; `--verify-only`
re-checks an existing tree (pair it with `--install-host-libs` too if you want
it to check the `tools/adhost/` copies rather than the assets-root ones).

## Verification

Every needed fork is checksummed against `assets.manifest.json` (md5 of the
exact resource/data forks the hosts read, plus the two shared libraries, plus
the two reference StuffIt archives). A clean run ends with:

```
=== manifest verify: 101/101 ok, 0 differ, 0 missing ===
```

If you fetched from an Internet Archive **disk image** rather than the reference
StuffIt, some per-file raw md5s may differ (a different distribution/build of
the same software) while still verifying `ok` — see "PPC resource forks and
`md5Canonical`" below. For a guaranteed *raw* byte match, supply the exact
StuffIt whose md5 equals the manifest's `sourceArchives` entry.

The two IA fallbacks, by direct comparison against the manifest:

- **Deluxe (68K) IA ISO** — byte-identical to the reference build: all 61 68K
  module rsrc forks + the shared "After Dark 4.0 Shared" data fork match `md5`
  exactly.
- **AD9 (PPC) IA image** — content-identical to the reference build, confirmed
  for 18 of the 19 PPC modules + the AD4 Library by direct `resource_dasm`
  decode diff (0 bytes differ across every decoded resource, every module) in
  a full sweep, plus Flying Toasters! separately in an earlier sample run
  (also 0 bytes differ), and by
  an end-to-end runtime test: Time Flies, Swirling Magic, and Rodger Dodger,
  freshly fetched from this image through `adfetch.sh` with no other input,
  all sustain motion on the runtime host identically to the reference build.
  Time Flies used to crash when read raw off this image; that was an
  extraction-pipeline bug, not a property of the archive. Its raw fork md5s
  do NOT match `md5` — every PPC module rsrc fork + the AD4 Library rsrc
  differ byte-for-byte from the reference — but the only differing bytes sit
  in the resource fork header's unused 240-byte "reserved for system use" pad
  (bytes 16-255, before resource DATA begins at byte 256; Inside Macintosh).
  That region is scratch space no Mac tool ever reads; this image and our
  reference build just carry different leftover extraction-tool buffer
  garbage there, not different content or compression.

### PPC resource forks and `md5Canonical`

Each PPC module's rsrc entry (and the AD4 Library) carries two hashes: `md5`
(the exact reference build) and `md5Canonical` (md5 after zeroing
resource-fork bytes 16-255). A fork verifies `ok` if it matches *either* one.
`adfetch.sh` canonicalizes (zeroes that pad) every PPC rsrc fork it places,
from either source, so a fresh fetch's md5 always equals `md5Canonical`; `md5`
is kept so trees populated before this change still verify without
re-fetching.

Both IA sources are Mac disk images read via `hfsutils` (see `sources.conf` for
the `hfsimg2`/`hfsiso` extraction modes) rather than plain archives `unar` can
open directly.

## Files

| file | purpose |
|------|---------|
| `adfetch.sh` | download → extract → place shared libs → verify |
| `sources.conf` | where each source comes from + how to unpack it |
| `assets.manifest.json` | md5 of every needed fork (generated from the reference build) |
| `verify_manifest.py` | standalone manifest checker (`verify_manifest.py <assets_root>`) |

## Regenerating the manifest

If you intentionally rebase onto a different asset build, regenerate the
manifest from a known-good populated tree:

```bash
# (script lives in this dir; see the header of assets.manifest.json)
python3 verify_manifest.py "$AD_ASSETS_DIR"     # confirm current state first
```
