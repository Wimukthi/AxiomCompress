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
