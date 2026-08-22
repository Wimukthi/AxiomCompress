# Versioning and releases

Axiom uses a four-part version number:

```text
major.minor.patch.build
```

The executable resource in `src\gui\axiom_gui.rc` is the **single source of
truth**. The About dialog reads the file version from that resource at runtime,
and the installer reads the same value when it names release files.

## When each part changes

| Part | Increment it when |
|---|---|
| `major` | Behaviour breaks compatibility, or the architecture changes substantially |
| `minor` | A user-visible feature lands that doesn't break existing behaviour |
| `patch` | Bug fixes, polish, reliability, and performance work |
| `build` | Every Visual Studio GUI build — MSBuild does this on its own |

Changing a part resets everything to its right:

- `0.1.0.14` → `0.1.1.0` for a patch release
- `0.1.1.8` → `0.2.0.0` for a minor feature release
- `0.9.4.3` → `1.0.0.0` for a major release

## Archive format compatibility

The executable version and the AXAR container version are separate contracts.
A newer Axiom must keep reading older archives; that obligation does not expire
when the product version goes up.

The format is **frozen**. The magic prefix, the header layout, the container
versions, and the eight assigned required-flag bits are fixed. A new required
flag may only take an unassigned bit from `0x0100` upward, and only for a
profile that stays off unless a user asks for it, so default options never begin
setting a required flag they did not already set. An archive written with
defaults by any 1.x release opens on 1.0. Anything that changes what a written
archive already contains needs AXAR v6 and a major release.

The baseline is **AXAR v4**, and readers must continue to open v4 archives
indefinitely. **AXAR v5** is an additive revision, used when a required fidelity
feature can't be represented safely in v4: sparse allocation maps, a
source-capture report, extended metadata, encryption-v2 password slots, snapshot
repositories, large solid blocks, and live deduplication. Legacy v4 encryption
stays readable forever.

### Required flags are requirements, not hints

An AXAR header feature flag means "you must understand this to read the file
correctly". A reader must **reject** a required flag it doesn't implement,
rather than trying its luck. A v4 header must never claim a v5-only flag.

| Flag | Profile |
|---:|---|
| `0x0001` | Encrypted central directory |
| `0x0002` | Sparse-file allocation maps |
| `0x0004` | Source-capture report |
| `0x0008` | Extended metadata (ACLs, xattrs, reparse points) |
| `0x0010` | Encryption-v2 password slots |
| `0x0020` | Snapshot chunk table and manifest |
| `0x0040` | Large solid blocks (LZMA2, above 4 GiB through 64 GiB) |
| `0x0080` | Live content deduplication (mutually exclusive with `0x0020`) |

The large-solid profile permits LZMA2 solid blocks above 4 GiB and up to 64 GiB
while keeping the pieces inside them bounded. It does not change the ordinary
v4/v5 block ceiling, and an existing archive never picks up the flag during a
normal read or update.

The snapshot profile's chunk table and named manifests are additive directory
extensions, but its live entry addresses are chunk references rather than one
contiguous range. A reader that doesn't understand flag `0x0020` must reject
the repository rather than attempt a restore that would be wrong.

Inside entry and archive extension areas, unknown TLV records stay safely
skippable, because every record carries its own bounded payload length. That's
what lets optional metadata evolve without touching the core record layout.

### Append generations

Phase-3 append generations are additive and do **not** introduce AXAR v6.

An append-compatible update keeps the previous archive bytes exactly where they
are and adds new blocks, a new directory, a 64-byte `AXIOMGF` generation
extension, optional recovery data, and the existing 24-byte footer shape. The
extension links the new directory back to the previous footer and generation.

A reader that understands the extension can scan backwards to the last valid
footer after an interrupted append. Older v4/v5 readers can still find the
current directory from an intact final footer, but they don't get the
generation history or the crash fallback.

Recovery data protects the generation extension as part of the new generation.
Signatures cover the canonical generation history, and signing an existing
archive creates a new generation. Anything that must change the header, or that
compacts the archive, keeps using the atomic temporary-file replacement path.

### Password slots

New password-protected archives encrypt data with a random archive key, which
is then wrapped by one or more independent password slots. Adding, changing, or
removing a slot rewrites the encryption metadata and the directory, and
preserves every compressed and encrypted block byte.

The first slot operation on a legacy v4 encrypted archive migrates its metadata
to the v5 scheme without re-encrypting anything. Password changes invalidate
signatures and rebuild recovery data against the new layout.

### Subframe maps

Plaintext AXAR blocks may carry an optional type-1 block-extra subframe map,
recording where independently decodable pieces begin. Selected extraction uses
it to fetch only the pieces it needs.

The map is additive, length-delimited directory data. It doesn't change the
header version or add a required flag, and a reader that doesn't implement it
can skip the block-extra area and decode whole blocks — still correct, just
slower. Maps are omitted for encrypted, transformed, serial, and otherwise
non-seekable blocks. Signed archives include the map in the canonical directory
digest.

### Testing the rules

Every profile the freeze covers has a checked-in golden fixture under
`tests/fixtures/`, exercised by `format_freeze_golden_profiles`: the v4
baseline, and v5 carrying a capture report, extended metadata, encryption-v2
password slots, live deduplication, and a snapshot chunk table. The test
asserts each fixture's magic, version, required-flag set, and reserved word,
then opens it. Regenerating a fixture to make the test pass would defeat it.

The large-solid-block profile (`0x0040`) has no golden archive, because a
representative one would exceed 4 GiB. It is covered by the header-level and
geometry validation tests instead.

Negative tests cover reserved fields, unknown required flags, a v4 header
claiming a v5-only flag, malformed TLV lengths, varint overflow, and
truncation. The frozen constants themselves are asserted at compile time in
`src/archive/container.cpp`.

Changing any of these rules requires a deliberate format review and a matching
update to [FORMAT.md](../FORMAT.md).

## The automatic build number

`AxiomSfx.vcxproj` runs `tools\Update-AxiomVersion.ps1` as a prerequisite,
before the product executables build. Putting it in that shared prerequisite is
what guarantees the SFX module, the GUI, and the CLI all see one version in a
single build.

The script increments only the fourth part, and updates every resource field
together — `FILEVERSION`, `PRODUCTVERSION`, `FileVersion`, and
`ProductVersion` — then mirrors the result into the CLI and SFX module
resources, so a generated self-extractor reports the same product version.

This deliberately modifies `src\gui\axiom_gui.rc`, which means a successful
local GUI build leaves a version change in your working tree. To build without
that:

```powershell
.\tools\build_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
```

or, driving MSBuild directly:

```powershell
MSBuild.exe .\AxiomCompress.sln /p:Configuration=Release /p:Platform=x64 /p:AutoIncrementVersion=false /m
```

Leave auto-increment on for day-to-day work. Turn it off for a release, so
packaging doesn't quietly turn a planned `0.1.1.0` into `0.1.1.1`.

## Making a release

1. Update `major`, `minor`, or `patch` in `src\gui\axiom_gui.rc` if the release
   warrants it, and set the full four-part version to the exact tag you intend
   to ship.
2. Add the release entry to [`CHANGELOG.md`](../CHANGELOG.md).
3. Build and test with auto-increment disabled:

   ```powershell
   .\tools\test_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
   ```

4. Package from the binaries you just built:

   ```powershell
   .\installer\build-installer.ps1 -SkipBuild -SkipTests -Version <version>
   ```

5. Build the matching portable zip.
6. Tag the release with the exact resource version.

### Before you push the tag

- `git status` shows only the intended release commit.
- `tools\Update-AxiomVersion.ps1 -PrintVersion` matches the proposed tag
  exactly.
- CI passes on Windows, Linux, and macOS for the tagged commit.

### After publishing

- The GitHub release is neither a draft nor a prerelease.
- Both Windows assets are attached.
- Their SHA-256 digests match the files you packaged locally.
