# Third-party software in AfterDark.app

AfterDark.app bundles a few programs it did not write. This file says what they
are, under what licence they are redistributed, and where to get their source.

Nothing here is an original After Dark file: the copyrighted After Dark modules
are never shipped with the app — the user downloads them on first run (see
`tools/adfetch/sources.conf`).

## Bundled at `AfterDark.app/Contents/Helpers/bin`

These are the archive-extraction tools the first-run downloader drives to unpack
the Internet Archive disk images. They are built from unmodified upstream
releases by `tools/get_extractors.sh` (pinned versions below), as universal
arm64 + x86_64 binaries, and code-signed with the app's identity.

| Binary | Project | Version | Licence |
|---|---|---|---|
| `unar` | [XADMaster](https://github.com/MacPaw/XADMaster) | `v1.10.8` | LGPL-2.1-or-later |
| (linked into `unar`) | [universal-detector](https://github.com/MacPaw/universal-detector) | `1.1` | MPL-1.1 / GPL-2.0-or-later / LGPL-2.1-or-later tri-licence |
| `hmount`, `hcd`, `hls`, `hcopy`, `humount` | [hfsutils](https://www.mars.org/home/rob/proj/hfs/) | `3.2.6` | GPL-2.0-or-later |

**Source.** Run `tools/get_extractors.sh`: it fetches exactly the sources these
binaries are built from —

* `git clone --branch v1.10.8 https://github.com/MacPaw/XADMaster.git`
* `git clone --branch 1.1 https://github.com/MacPaw/universal-detector.git`
* `https://ftp.mars.org/hfs/hfsutils-3.2.6.tar.gz`
  (sha256 `bc9d22d6d252b920ec9cdf18e00b7655a6189b3f34f42e58d5bb152957289840`;
  mirror `https://ftp2.osuosl.org/pub/clfs/conglomeration/hfsutils/hfsutils-3.2.6.tar.gz`)

and leaves the complete, unpacked source trees — including each project's own
licence text — under `third_party/extractors/src/`. Anyone with a copy of the
app can therefore obtain, modify, and rebuild these tools.

**Build-only changes.** The tools are functionally unmodified. Three edits let
decades-old code build with a current toolchain, and `get_extractors.sh` applies
them in the open (they are the same ones Homebrew's own `unar` and `hfsutils`
formulae carry):

* XADMaster: the Xcode project names `libstdc++.6.dylib`, which no longer ships
  with macOS → `libc++.1.dylib`.
* XADMaster: the `__DATE__` in the version banner is replaced with a fixed
  string, so builds are reproducible.
* hfsutils: `hpwd.c` calls `strcmp` without including `<string.h>`, and the
  package predates C99's removal of implicit `int` → add the include, compile
  with `-Wno-implicit-int -Wno-implicit-function-declaration`.

## Built into the emulation hosts (`Contents/Helpers/adhost`, `adhost68k`)

| Project | Licence | Source |
|---|---|---|
| [resource_dasm](https://github.com/fuzziqersoftware/resource_dasm) (resource/PEF decoding, m68k + PPC emulators) | MIT | `third_party/resource_dasm` |
| [phosg](https://github.com/fuzziqersoftware/phosg) (its support library) | MIT | `third_party/phosg` |

## macOS itself

`curl`, `awk`, `dd`, `md5`/`openssl` and `bash` come with macOS and are used
as installed. The first-run download deliberately needs nothing else: no
Homebrew, and no `python3`.
