# Versioning

Axiom uses a four-part version:

```text
major.minor.patch.build
```

The executable resource in `src\gui\axiom_gui.rc` is the **source of truth**.
The About dialog reads the file version from that resource at runtime, and the
installer reads the same value when naming release artifacts.

## Rules

| Component | Increment when |
|---|---|
| `major` | Compatibility-breaking behavior or a major architecture change |
| `minor` | User-visible features that preserve existing behavior |
| `patch` | Bug fixes, polish, reliability, and performance work |
| `build` | Every Visual Studio GUI build — MSBuild does this automatically |

When a component changes, reset everything to its right:

- `0.1.0.14` → `0.1.1.0` for a patch release
- `0.1.1.8` → `0.2.0.0` for a minor feature release
- `0.9.4.3` → `1.0.0.0` for a major release

## AXAR format compatibility

The executable version and the AXAR container version are separate contracts.
The current archive baseline is AXAR v4, and readers must continue to open v4
archives. AXAR v5 is an additive revision used when a required fidelity feature
cannot be represented safely by the v4 baseline; this includes the encryption-v2
password-slot, snapshot-repository, and large-solid-block profiles. Legacy v4
encryption remains readable indefinitely.

The large-solid profile uses required header flag `0x0040`. It permits AXAR
LZMA2 solid blocks from above 4 GiB through 64 GiB while keeping the AXEC
subframes bounded; it does not change the ordinary v4/v5 block ceiling. A
reader that does not implement the profile must reject the required flag, and
older archives never acquire it during normal reads or updates.

Phase-3 append generations are additive and do not introduce AXAR v6. An
append-compatible update retains the previous complete archive bytes and adds
new blocks, a directory, a version-1 64-byte `AXIOMGF` generation extension,
optional recovery data, and the existing 24-byte footer shape. The extension
links the new directory to the previous footer and generation. Readers that
understand the extension can scan backward to the previous valid footer after a
torn append; older v4/v5 readers can still locate the current directory from an
intact final legacy footer, but do not expose generation history or fallback.

Recovery protects the generation extension as part of the new directory
generation. Signatures include the canonical generation history, and signing
an existing archive creates a new generation. Operations that require header
changes or compaction continue to use the atomic temporary-replacement path.

New password-protected archives use a random archive data key wrapped by one or
more independent password slots. Adding, changing, or removing a slot rewrites
the encryption metadata and directory while preserving compressed/encrypted
block bytes. The first slot operation on a legacy v4 encrypted archive migrates
its metadata to the v5 encryption-v2 profile without re-encrypting those blocks.
Password mutations invalidate signatures and rebuild recovery data against the
new layout.

The AXAR v5 snapshot repository profile uses required header flag `0x0020`.
Its chunk table and named snapshot manifest are additive directory extensions,
but the live entry addresses are chunk references rather than one contiguous
solid-file range. A v4 reader, or any reader that does not understand this
required flag, must reject the repository instead of attempting a potentially
incorrect restore. Snapshot generations use the existing `AXIOMGF` append
extension; pruning and adding snapshots therefore retain the same crash
fallback and recovery rules as other append-compatible metadata updates.

Plaintext AXAR blocks may also carry an optional type-1 block-extra subframe
map. It records entry-independent frame boundaries for the parallel AXC and
external-codec envelopes, allowing selected extraction to fetch only the
intersecting compressed frames. The map is additive, length-delimited directory
data: it does not change the AXAR v4/v5 header version or required feature flags,
and readers that do not implement it can skip the block extra area. Maps are
omitted for encrypted, transformed, serial, or otherwise non-seekable blocks.
Signed archives include the map in the canonical directory digest. A reader
that cannot use a map must remain correct by decoding the complete solid block.

AXAR header feature flags are required capabilities, not hints. A reader must
reject an unknown required flag, and a v4 header must not claim a v5-only flag.
Within entry and archive extension areas, unknown TLV records remain safely
skippable because every record carries its own bounded payload length. This
allows optional metadata to evolve without changing the core record layout.

Every format revision is covered by a checked-in golden fixture under
`tests/fixtures/`, plus negative tests for reserved fields, unknown required
flags, malformed TLV lengths, and truncation. Changes to these rules require a
deliberate format review and an update to `FORMAT.md`.

## Automatic build increment

The `AxiomSfx.vcxproj` prerequisite runs `tools\Update-AxiomVersion.ps1` before
the product executables build. Running it in that shared prerequisite
guarantees the embedded SFX module, GUI, and CLI all see the same version in
one build.

The script increments only the fourth component and updates every resource
field together — `FILEVERSION`, `PRODUCTVERSION`, `FileVersion`, and
`ProductVersion` — then mirrors the result into the CLI and SFX-module
resources, so generated self-extractors report the same product version.

This deliberately modifies `src\gui\axiom_gui.rc`, so a successful local GUI
build leaves a version change in the working tree.

To build without touching the version:

```powershell
.\tools\build_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
```

or, driving MSBuild directly:

```powershell
MSBuild.exe .\AxiomCompress.sln /p:Configuration=Release /p:Platform=x64 /p:AutoIncrementVersion=false /m
```

Keeping auto-increment on is fine for day-to-day builds. Disabling it for
releases avoids turning a planned `0.1.1.0` into `0.1.1.1` during packaging.

## Release procedure

1. Update `major`, `minor`, or `patch` in `src\gui\axiom_gui.rc` if the release
   warrants it, and set the full four-part version to the exact tag you intend
   to ship.
2. Update [`CHANGELOG.md`](../CHANGELOG.md) with the release entry.
3. Build and test with auto-increment disabled:

   ```powershell
   .\tools\test_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
   ```

4. Package from the already-built binaries:

   ```powershell
   .\installer\build-installer.ps1 -SkipBuild -SkipTests -Version <version>
   ```

5. Build the matching portable zip asset.
6. Tag the release with the exact resource version.

### Before pushing the tag

- `git status` contains only the intended release commit.
- `tools\Update-AxiomVersion.ps1 -PrintVersion` exactly matches the proposed
  tag.
- CI passes on Windows, Linux, and macOS for the tagged commit.

### After publishing

- The GitHub release is neither draft nor prerelease.
- Both Windows assets are present.
- Their SHA-256 digests match the locally packaged files.
