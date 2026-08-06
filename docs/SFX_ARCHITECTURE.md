# How the self-extractor works

Design and audit notes for Axiom's configurable Windows self-extractor.

A self-extracting archive is your archive glued onto a small program, so the
whole thing is one `.exe` the recipient can just run. Axiom's is an
**extractor, not an installer**: it puts archive bytes on disk, optionally gates
that on a license, and can run a program that came out of the archive. Nothing
more.

The shipped command and configuration contract is documented in
[CLI_GUIDE.md](../CLI_GUIDE.md#self-extracting-archives). The archive wrapper
bytes are in [FORMAT.md](../FORMAT.md). This document is about why the design
looks the way it does.

## Scope

**In scope:** presentation, destination control, a license gate, selective
extraction, silent and scripted operation, and running a program that came out
of the archive.

**Out of scope:** shortcuts, registry writes, uninstallers, component trees, and
custom actions.

That line is drawn deliberately. An SFX that leaves system state behind is an
installer, and installers need rollback, repair, upgrade detection, and
per-machine versus per-user policy — a far larger commitment than this design
makes. Use Inno Setup for that. Axiom's job is to get the bytes onto disk
correctly.

The one deliberate exception is **run-after-extract**, because it is what makes
an SFX usable as a bootstrapper for a real installer. It is constrained in
[Security model](#security-model) so it cannot become a general-purpose
execution primitive.

## Why the format had to change

### The signing blocker

**(v1)** The SFX trailer was the last 16 bytes of the file: eight magic bytes
`AXIOMSFX` followed by a little-endian `u64` payload length. Both the runtime
(`src/sfx/runtime.cpp`) and the archive engine
(`sfx_embedded_payload_range()` in `src/archive/container.cpp`) located the
payload by seeking 16 bytes back from the end of the file.

Authenticode stores its certificate as trailing bytes at the physical end of the
PE, pointed to by the Security entry of the data directory. On a signed binary
the certificate table ends exactly at EOF:

```text
Rar.exe   cert_off=825344  cert_size=10448  file=835792   cert_off+cert_size == file
```

So an EOF-anchored trailer and Authenticode are mutually exclusive. There is no
ordering that works:

| Order | Result |
|---|---|
| Sign the stub, then append the payload | The appended bytes are hashed but are not the certificate table, so the signature no longer validates |
| Append the payload, then sign | `signtool` appends the certificate after the trailer; the trailer is no longer at EOF−16, and the runtime reads certificate bytes instead of `AXIOMSFX` |

An unsignable executable is permanently SmartScreen-flagged. For a file whose
documented purpose is `release-setup.exe`, that is a serious limitation, and it
is the primary reason for a v2 format.

### Everything else the old format couldn't express

The trailer carried a length and nothing else. No version field, so the format
couldn't evolve. No integrity field, so a truncated or spliced payload was only
caught later, by the archive layer, as a confusing error. Nowhere to put
configuration, so every SFX Axiom produced behaved identically.

## Where the payload lives, in v2

Stop anchoring on EOF. Anchor on the **end of the PE image**, which is
computable from the headers and is unaffected by anything appended later.

```text
+-------------------+  0
|   PE stub image   |
+-------------------+  image_end  = max(PointerToRawData + SizeOfRawData)
|  SFX descriptor   |             after the last raw section byte
+-------------------+
|   config blob     |  (optional, TLV)
+-------------------+
|  archive payload  |  intact .axar or .zip
+-------------------+
| certificate table |  (optional, added by signtool afterwards)
+-------------------+  EOF
```

A reader parses `e_lfanew`, walks the section table, computes `image_end`, and
reads the descriptor there. Nothing in that path depends on the file's total
length, so appending a certificate is invisible to it.

```text
unsigned stub  image_end == file size
signed SFX     image_end < certificate offset <= file size
```

The packager requires the supplied stub to contain exactly the PE image. An
overlay is rejected rather than silently copied or discarded, so a certificate
can only be added after the finished SFX is produced.

This also buys something v1 could never offer. Authenticode hashes everything
from the end of the headers to the end of the file *except* the certificate
table and its directory entry — so trailing data is covered. Signing a v2 SFX
therefore authenticates **the archive payload as well as the stub**. The
signature attests to the contents, not just to the extractor.

### The descriptor

Fixed 64 bytes, little-endian, at `image_end`:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Magic `AXSFX2\0\0` |
| 8 | 2 | `format_version` (2) |
| 10 | 2 | `header_size` (64, for forward growth) |
| 12 | 4 | `flags` |
| 16 | 8 | `config_offset`, relative to `image_end` |
| 24 | 8 | `config_size` |
| 32 | 8 | `payload_offset`, relative to `image_end` |
| 40 | 8 | `payload_size` |
| 48 | 8 | Payload BLAKE3-256, first 8 bytes |
| 56 | 4 | Reserved, must be zero |
| 60 | 4 | CRC-32 of bytes 0..59 |

The descriptor CRC makes a corrupt or partially written SFX fail immediately
with a clear message, rather than surfacing later as a confusing archive-layer
error. It costs 64 bytes to check, so it is always checked.

The payload hash is **not** verified on every launch. Hashing the whole payload
would add a full extra read before extraction could start — noticeable on a
large archive, and largely redundant, since the archive layer already verifies
per-block CRC-32 and per-file BLAKE3 while extracting. The hash exists so
`--test` and external tooling can confirm a payload end to end without
decompressing it, and to catch a truncated download early.

It is a corruption check, not a security control. The
[signature](#security-model) is the security control.

### Reading order, and v1 compatibility

`sfx_embedded_payload_range()` does this, in order:

1. Parse the PE headers. If `image_end` holds a valid `AXSFX2` descriptor whose
   CRC checks out, use it. If the file carries a certificate table, treat
   `min(EOF, cert_offset)` as the logical end when range-checking.
2. Otherwise fall back to the v1 EOF−16 `AXIOMSFX` trailer.
3. Otherwise it isn't an SFX.

Existing v1 executables keep working in the GUI browser, in `axiomc`, and as
standalone extractors. Every v1 SFX already in the wild carries its own v1
runtime, so nothing in the field needs the new reader. `axiomc sfx` emits v2
unconditionally — there is no reason to offer v1 output.

## Configuration

Two representations, deliberately:

- **Authoring:** a small INI-style text file passed to `axiomc sfx --config`.
  Human-writable, diffable, reviewable.
- **On disk:** a TLV blob. `axiomc` parses the text at creation time and emits
  TLV; the stub only ever parses TLV.

The stub parses input that arrives with an untrusted executable, so it should
carry the smallest parser that will do the job. A text parser in the stub would
be a larger attack surface and a larger binary for no benefit.

Unknown TLV tags are **rejected**, not skipped. A stub that silently ignored a
`require_admin` or `require_accept` it didn't understand would be quietly
downgrading the author's intent. Failing loudly is correct.

### Keys

| Group | Keys |
|---|---|
| Presentation | `title`, `window_title`, `description`, `banner_text`, `theme` (`auto`\|`light`\|`dark`) |
| Destination | `default_path`, `allow_path_change`, `create_subfolder` |
| License | `license_text`, `require_accept` |
| Behaviour | `mode` (`interactive`\|`silent`\|`very_silent`), `overwrite`, `restore_mtime`, `open_destination`, `auto_close`, `threads` |
| Selection | Command-line only, through repeatable `--include`. `allow_file_selection` is reserved and rejected when true |
| Run after | `run_program`, `run_arguments`, `run_working_dir`, `wait_for_exit`, `propagate_exit_code` |
| Elevation | `elevation` (`none`\|`auto`\|`require`) |

`threads` accepts `0` for automatic selection, or 1 through 4096. Text fields
are capped at 1 MiB, path-like fields at 32,767 bytes, and the encoded
configuration at 4 MiB. Duplicate keys and duplicate TLV tags are rejected. Text
must be valid UTF-8, and embedded NULs are refused.

### Path templates

`default_path` expands `%ProgramFiles%`, `%ProgramFiles(x86)%`,
`%LOCALAPPDATA%`, `%APPDATA%`, `%USERPROFILE%`, `%DESKTOP%`, `%DOCUMENTS%`,
`%TEMP%`, plus `%SFXDIR%` (the folder holding the executable) and `%SFXNAME%`
(its stem).

Shell locations resolve through `SHGetKnownFolderPath`, never through
environment variables, so a caller cannot redirect `%ProgramFiles%` by setting
an environment variable before launching the SFX.

After expansion the result must be absolute and must contain no `..` component.
Anything else is a hard error at startup, not a silently corrected path.

## Command line

The stub has a real argument parser. **(v1)** treated `argv[1]` as the
destination and offered no options at all.

```text
setup.exe [options] [destination]

  -o, --output <dir>        destination directory
  -s, --silent              progress and errors only, no prompts
      --very-silent         no window at all
  -y, --accept              accept the license, assume yes
  -p, --password <text>     password for an encrypted payload
      --password-stdin      read the password from stdin
      --overwrite <replace|skip|fail>
      --threads <n>         0 for automatic, otherwise 1..4096
      --include <pattern>   extract only matching entries, repeatable
      --no-run              extract but do not run run_program
      --list                print the contents and exit
      --test                verify payload integrity and exit
      --log <file>          append a UTF-8 transcript (never the SFX itself)
  -?, --help
```

Command-line options override the config blob, with two exceptions:
`require_accept` and `elevation=require` cannot be relaxed from the command
line. See [Security model](#security-model). `--silent` without `-y` on a
payload that requires acceptance is a usage error, not an implied acceptance.

### Console output

The stub stays a `WINDOWS` subsystem binary, so double-clicking never flashes a
console. For `--list`, `--test`, `--help`, and silent-mode diagnostics it calls
`AttachConsole(ATTACH_PARENT_PROCESS)` and writes to the inherited handles when
that succeeds, falling back to a message dialog when it doesn't.

This is the standard approach, and it's why `cmd` gets output while Explorer
stays clean.

### Exit codes

**(v1)** returned 0 for both success and user cancellation, and 1 for both
failure and "not an SFX" — unusable in a script.

| Code | Meaning |
|---:|---|
| 0 | Success |
| 1 | Extraction failed |
| 2 | Bad usage |
| 3 | Cancelled by the user |
| 4 | Wrong or missing password |
| 5 | Integrity or signature check failed |
| 6 | Not enough free space at the destination |
| 7 | Elevation required and unavailable or refused |
| 8 | Extraction succeeded but `run_program` failed to start |
| >8 | With `propagate_exit_code`, the exit code of `run_program` |

## Runtime architecture

### Read the payload where it lies

The runtime passes the executable path directly to a read-only
`SfxArchiveReader`. AXAR uses the located payload range; the ZIP reader handles
the wrapped executable path. Both paths are exercised by the end-to-end SFX
tests.

The reader facade deliberately exposes only capabilities, listing, testing,
signature inspection, and extraction — so the Mini stub cannot inherit the
archive mutation and provider registry, or the encoder backends.

No temporary full-size archive copy is part of the normal extraction path. Any
future reader that genuinely requires a temporary copy must use
`FILE_FLAG_DELETE_ON_CLOSE`, so the kernel reclaims it even after a hard kill.

### Startup sequence

```text
parse args
  -> locate + validate descriptor        (fail fast, exit 5)
  -> read + parse config TLV             (fail fast, exit 5)
  -> open payload in place
  -> password if encrypted               (exit 4)
  -> verify archive signature            (exit 5)
  -> license gate                        (exit 3)
  -> destination + selection UI          (exit 3)
  -> resolve elevation, relaunch if needed
  -> free-space precheck                 (exit 6)
  -> extract with progress
  -> run_program                         (exit 8 / propagated)
```

`--test` performs the extra full-payload BLAKE3-prefix check before asking the
archive provider to test the payload. Normal extraction validates the archive
through its provider while reading, without paying for a second full payload
read first.

The free-space precheck compares the archive's unpacked size against
`GetDiskFreeSpaceExW` on the destination volume. It turns a failure that would
otherwise happen partway through a long extraction into an immediate,
actionable error.

### Elevation

`elevation=auto` resolves at startup by testing whether the destination is
actually writable — creating and deleting a probe file — rather than by
pattern-matching the path against `Program Files`. If it isn't writable and the
process isn't elevated, the stub relaunches itself with the `runas` verb,
forwarding the original arguments plus the resolved destination.

`elevation=require` always relaunches. `elevation=none` never does, and fails
with exit 7 if the destination isn't writable. The original process waits for
the elevated child and returns its exit code, so a script never sees a false
success when the child fails or is cancelled.

A password supplied via `--password` must not be forwarded on the relaunched
command line, where it would be visible in the process list. When a relaunch is
needed and a password came from the command line or stdin, the first process
refuses the combination outright. With the full stub, a password entered
interactively is cleared and the elevated instance prompts again.

### Stub tiers

Both tiers run the same `runtime.cpp`, differing only in their `SfxUi`
implementation: `DialogUi` for the full stub, `ConsoleUi` for the Mini one. That
interface is the only reason the Mini stub can avoid linking the Win32 dialog
stack at all.

Select with `axiomc sfx --stub full|mini`. The Mini stub suits a build artifact
unpacked by a script; the full stub is for something a person double-clicks.

**Measured on the current Release x64 build (0.9.2.2):**

| Binary | Size | What it contains |
|---|---:|---|
| `AxiomSfxMini.bin` | 794,112 bytes (776 KiB) | Decode-only AXAR/ZIP runtime plus console UI |
| `AxiomSfx.bin` | 2,324,480 bytes (2.22 MiB) | Full SFX UI plus compatibility archive services |

Mini is 65.8% smaller than the full stub. `tools/test_sfx_footprint.ps1` guards
that with a 1 MiB maximum and a minimum 20% reduction from the full image.

The original under-400 KB target turned out to be too aggressive for the
compatibility surface being supported, but Mini is now a genuinely separate
decode runtime rather than a trimmed copy. The full stub still links the general
archive library for its archive-browser dialog compatibility path; its
extraction runtime uses the same read-only facade as Mini.

Portable and installed Axiom packages ship both module files beside `Axiom.exe`
and `axiomc.exe`. Omitting either file makes that tier unavailable, even though
the other tier still works.

The decode-only library (`AxiomSfxDecodeLib`) keeps the behaviour users expect
from an SFX while excluding write-side code:

- AXAR v1 and v2 payload location, listing, testing, signature inspection,
  comments, encryption prompts, and safe extraction remain available.
- Wrapped ZIP payloads retain listing, testing, and extraction. The same decode
  library also keeps standalone split-ZIP reader support.
- Encoder backends, archive writers, mutation operations, provider
  registration, 7-Zip adapters, and GUI dialogs are not linked into Mini.

Further reduction is possible, but it would require an explicit compatibility
trade-off — dropping ZIP support, split-volume reading, recovery, or signature
verification. Those are decisions to be made deliberately, not accidental
linker side effects.

## Security model

The threat model starts from an uncomfortable fact: running an SFX means running
an untrusted executable. The config blob is therefore **not** a trust boundary,
because an attacker who can write the config can equally write the code.

The design's obligation is narrower, and worth stating plainly: avoid making
things *worse* than that baseline, and keep the guarantees the archive format
already provides.

- **Extraction stays inside the destination.** The existing path-safety rules —
  no absolute paths, no `..` traversal, no reparse-point escapes — apply
  unchanged and remain covered by the round-trip suite.
- **`run_program` resolves only inside the destination.** It names a path
  relative to the extraction root, rejects absolute, drive-rooted, `..`, and
  alternate-data-stream forms, rejects symlink and reparse-point components, and
  requires a regular file the extraction actually produced. `run_working_dir`
  follows the same containment rules and must be a real directory. These
  settings are not a way to launch arbitrary system binaries.
- **Silent plus elevated plus run-after is refused.** `--very-silent` combined
  with `elevation=require` and a `run_program` is exactly the shape of an
  unattended privileged execution chain. It is a usage error.
- **The license gate cannot be bypassed from the command line.**
  `require_accept` needs an explicit `-y`, which is a record of acceptance, not
  a default.
- **Signature verification stays mandatory.** A payload carrying an invalid
  signature refuses to extract.
- **Input is bounded before it reaches the extractor.** PE geometry, descriptor
  version, flags, and reserved fields, descriptor ranges, config size, TLV
  lengths, duplicate tags, enum and boolean values, UTF-8 validity, worker
  counts, and archive/stub aliases are all validated. SFX output is installed
  through a unique sibling temporary and a replacement step, preserving the
  prior output if installation fails.
- **Authenticode becomes possible but stays the caller's job.** `axiomc sfx`
  does not shell out to `signtool`. It produces a file that can be signed
  afterwards, and the documentation states that ordering.

## Testing

- Descriptor round-trip across representative config permutations, verified by
  `--test` and `--list` plus a full silent extraction compared by hash.
- Malformed PE geometry, descriptor flag/reserved/header fields, overlapping
  config and payload ranges, duplicate TLV fields, invalid UTF-8, oversized
  worker counts, unsafe run paths, and empty or aliased packaging inputs.
- **The Authenticode regression test**, `tools/test_sfx_authenticode.ps1`. CI
  builds an SFX, signs it with a throwaway self-signed code-signing certificate,
  validates the certificate, runs `--test`, and extracts the payload. This
  proves the certificate table is ignored by the v2 locator, and it would have
  caught the v1 limitation.
- `tools/test_sfx_footprint.ps1` checks that Mini stays smaller than the full
  stub, below the 1 MiB release budget, and at least 20% smaller than the full
  image.
- `tools/test_sfx_runtime.ps1` packages and extracts AXAR v2, wrapped ZIP, and
  legacy AXAR v1 payloads through the Mini image, covering list, test, and
  silent extraction.
- v1 compatibility: a stored v1 SFX fixture must remain readable by the new
  locator.
- A fuzz target for the descriptor and TLV parsers, alongside the existing
  decode-surface targets.
- Path-template expansion under a hostile environment block, asserting that
  `%ProgramFiles%` cannot be redirected.

## Phases

| Phase | Contents | State |
|---:|---|---|
| 1 | v2 descriptor and PE-aware locator, v1 fallback, payload hash, in-place payload read, signing support | Landed |
| 2 | Argument parser, exit-code contract, `--list`/`--test`, silent modes, console attach | Landed |
| 3 | Config blob, presentation, destination templates, license gate | Landed |
| 4 | Selective extraction, run-after-extract, elevation, free-space precheck | Landed |
| 5 | `--stub full\|mini`, console tier, transcript logging, bounded validation | Landed |
| 6 | Decode-only AXAR/ZIP runtime, encoder exclusion, Mini footprint guard | Landed |

Phase 1 fixed signing and removed the temporary copy without changing any
user-visible behaviour. `create_sfx_archive` now rejects a stub that isn't a PE
image, and one that is already signed — where before it would append to
anything.

Phase 5 shipped the tier split and the `SfxUi` interface. Phase 6 moved the
archive read surface behind `SfxArchiveReader`, removed write-side dependencies
from Mini, and added the footprint regression guard. See
[Stub tiers](#stub-tiers).
