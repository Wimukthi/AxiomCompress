# Third-party notices

AxiomCompress is distributed under the [GNU General Public License v3](LICENSE).
The components below are bundled with it and retain their own licenses.

## Summary

| Component | Version | License | Used for |
|---|---|---|---|
| [Wimukthi.Win32Theme](#wimukthiwin32theme) | pinned commit | MIT | Native Windows theme integration |
| [Darkmodelib](#darkmodelib) | 0.75.0 | MPL-2.0, with MIT parts | Dark mode, via the theme framework |
| [Zstandard](#zstandard) | 1.5.7 | BSD-3-Clause | Optional AXAR Zstandard block codec |
| [LZMA SDK](#lzma-sdk) | 26.02 | Public domain | Optional AXAR LZMA2 block codec |
| [miniz](#miniz) | 3.1.2 | MIT | ZIP container read/write and Deflate |
| [minizip-ng](#minizip-ng) | 4.2.2 | zlib | Standard split-ZIP container core |
| [7-Zip engine](#7-zip-engine) | 26.02 | LGPL-2.1+, BSD parts, unRAR restriction | Read-only 7z/RAR/ISO/CAB support |
| [Monocypher](#monocypher) | 4.0.3 | BSD-2-Clause / CC0-1.0 | Argon2id, XChaCha20-Poly1305, EdDSA |
| [BLAKE3](#blake3) | 1.8.5 | CC0-1.0 or Apache-2.0 | Content hashing and integrity |
| [Fluent UI System Icons](#fluent-ui-system-icons) | icon subset | MIT | GUI toolbar icons |

Source-tree notices live under `src/third_party/` and `third_party/`. Binary
packages carry them under `licenses/`, plus the installed `backends/` folder
for the 7-Zip engine.

## Wimukthi.Win32Theme

Axiom's Windows theme integration uses `Wimukthi.Win32Theme`, an MIT-licensed
C++20 facade maintained alongside this repository.

- <https://github.com/Wimukthi/Wimukthi.Win32Theme>
- Pinned commit: `f39163076210144c492f79c82979218a10553bfb`

Binary packages include its MIT license as
`licenses/Wimukthi.Win32Theme-LICENSE.txt` and its own notices as
`licenses/Wimukthi.Win32Theme-NOTICES.md`.

## Darkmodelib

`Wimukthi.Win32Theme` vendors Darkmodelib 0.75.0.

- <https://github.com/ozone10/win32-darkmodelib>
- Pinned commit: `fa99647299c4edb3cf662bc14f19b5451090723e`

Darkmodelib is primarily licensed under the Mozilla Public License 2.0 and
contains files under the MIT license, with further notices documented in its
source tree.

MPL-2.0 requires that the corresponding source be made available. Binary
packages therefore include both license files
(`licenses/Darkmodelib-MPL-2.0.txt`, `licenses/Darkmodelib-MIT.txt`) and the
complete corresponding vendored source under `licenses/source/darkmodelib/`.

## Zstandard

Provides the optional AXAR Zstandard block method. Built from the official C
sources with assembly disabled for portable Release and CI builds.

- License: `src/third_party/zstd/LICENSE`, packaged as
  `licenses/Zstandard-BSD.txt`
- Copyright Meta Platforms, Inc. and affiliates

## LZMA SDK

Provides the optional AXAR LZMA2 block method. Built single-threaded inside
each AXC chunk, because Axiom owns the operation-wide worker budget and
parallelism.

- License: `src/third_party/lzma-sdk/LICENSE.txt`, packaged as
  `licenses/LZMA-SDK-Public-Domain.txt`
- The bundled C sources are public domain, by Igor Pavlov

## miniz

Provides the ZIP container reader/writer and the Deflate/Inflate
implementation, which the AXAR Deflate method also uses.

- Version: 3.1.2, pinned in `dependencies.lock.json`
- License: `src/third_party/miniz/LICENSE` (MIT)

## minizip-ng

A privately namespaced subset of minizip-ng's container and split-stream core,
used to create and read standard `.z01`, `.z02`, …, `.zip` sets while
raw-copying completed entries. The vendored split writer carries a documented
local-header boundary fix, verified against bundled 7-Zip.

- License: `src/third_party/minizip-ng/LICENSE` (zlib)

## 7-Zip engine

Axiom loads the 64-bit 7-Zip engine DLL directly, so read-only support for 7z,
RAR/RAR5, ISO/UDF, and CAB works on Windows without launching a console helper
or requiring 7-Zip to be installed.

- Version: 26.02, pinned in `dependencies.lock.json`
- Shipped: `backends/7zip/7z.dll`, `License.txt`, `readme.txt`
- Copyright © 1999-2026 Igor Pavlov
- Mainly GNU LGPL, with BSD-licensed parts and the upstream **unRAR
  restriction** on some RAR code — that code may not be used to develop a RAR
  (WinRAR) compatible archiver
- Upstream: <https://www.7-zip.org/>

`third_party/7zip/win-x64/7z.exe` exists in the source tree only as a
test-fixture generator for interoperability tests. It is not included in Axiom
installers or portable packages.

## Monocypher

Provides Axiom's cryptographic primitives: Argon2id key derivation,
XChaCha20-Poly1305 authenticated encryption, and EdDSA signatures.

- License: header notice in `src/third_party/monocypher/monocypher.h`
  (BSD-2-Clause or CC0-1.0)
- Version: 4.0.3, pinned in `dependencies.lock.json`
- Upstream: <https://github.com/LoupVaillant/Monocypher>

Note that Monocypher's EdDSA uses a BLAKE2b-based primitive and is **not**
wire-compatible with standard SHA-512 Ed25519.

## BLAKE3

Provides the per-file BLAKE3-256 content hashes and related integrity
primitives, with x86 SIMD backends selected at runtime by CPUID.

- License: dual CC0-1.0 / Apache-2.0, per the upstream project
- Upstream: <https://github.com/BLAKE3-team/BLAKE3>

## Fluent UI System Icons

The GUI toolbar uses alpha masks generated from a pinned subset of Microsoft's
Fluent UI System Icons. The SVG masters and the mapping from Axiom's names to
upstream icon names are in `assets/icons/fluent/`.

- License: `assets/icons/fluent/LICENSE` (MIT)
- Upstream: <https://github.com/microsoft/fluentui-system-icons>
