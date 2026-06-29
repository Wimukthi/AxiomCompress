# Versioning

Axiom uses the same four-part version format as NativePad:

```text
major.minor.patch.build
```

The executable resource in `src\gui\axiom_gui.rc` is the source of truth. The
About dialog reads the file version from that resource at runtime, and the
installer reads the same value when naming release artifacts.

## Short version

- Change `major`, `minor`, or `patch` manually when preparing a release that
  deserves it.
- Let the Visual Studio GUI build increment `build`.
- Use the exact resulting version for the installer name and GitHub release tag.
- Use `/p:AutoIncrementVersion=false` for diagnostic builds that must leave the
  working tree unchanged.

## Increment Rules

- Increment `major` for compatibility-breaking behavior or major architecture
  changes.
- Increment `minor` for user-visible features that preserve existing behavior.
- Increment `patch` for bug fixes, polish, reliability work, and performance
  improvements.
- Increment `build` for every Visual Studio GUI build. MSBuild does this
  automatically for `AxiomGui.vcxproj`.

When a parent component changes, reset the components to its right. Examples:

- `0.1.0.14` -> `0.1.1.0` for a patch release.
- `0.1.1.8` -> `0.2.0.0` for a minor feature release.
- `0.9.4.3` -> `1.0.0.0` for a major release.

## Automatic Build Increment

`AxiomGui.vcxproj` runs `tools\Update-AxiomVersion.ps1` before the app project
builds. The script increments only the fourth component and updates all resource
fields together:

- `FILEVERSION`.
- `PRODUCTVERSION`.
- `FileVersion`.
- `ProductVersion`.

This intentionally modifies `src\gui\axiom_gui.rc`, so a successful local GUI
build leaves a version change in the working tree.

To run a diagnostic build without changing the version:

```powershell
MSBuild.exe .\AxiomCompress.sln /p:Configuration=Release /p:Platform=x64 /p:AutoIncrementVersion=false /m
```

Before producing a packaged release build:

1. Manually update `major`, `minor`, or `patch` in `src\gui\axiom_gui.rc` if the
   release requires it.
2. Build Release x64 and let MSBuild increment the `build` component.
3. Run the Release test binary.
4. Build the Inno Setup installer.
5. Tag the release with the exact resource version.
