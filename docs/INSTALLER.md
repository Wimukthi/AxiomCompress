# Installer

Axiom uses Inno Setup 6 for the Windows installer, matching NativePad's release
packaging model.

The installer uses Inno Setup's dynamic modern wizard style, so Setup and
Uninstall follow the user's Windows light/dark app mode.

## Quick path

For a normal local package build:

```powershell
winget install --id JRSoftware.InnoSetup --exact
.\installer\build-installer.ps1
```

The script builds Axiom, runs the Release test executable, reads the version from
`src\gui\axiom_gui.rc`, and writes the installer to `installer\output`.

For a tagged release where the resource version has already been pinned, build
and test first with auto-increment disabled, then package without rebuilding:

```powershell
.\tools\test_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
.\installer\build-installer.ps1 -SkipBuild -SkipTests -Version <version>
```

## Local Requirements

- Release x64 build tools from Visual Studio.
- Inno Setup 6.

Install Inno Setup locally with:

```powershell
winget install --id JRSoftware.InnoSetup --exact
```

## Build Locally

From the repository root:

```powershell
.\installer\build-installer.ps1
```

Output:

```text
installer\output\AxiomSetup-<version>-win-x64.exe
```

To package an already-built Release binary:

```powershell
.\installer\build-installer.ps1 -SkipBuild -SkipTests
```

If `-Version` is supplied, it must match the current `src\gui\axiom_gui.rc`
version. This keeps the installer name, release tag, Windows file properties,
About dialog, and uninstall metadata aligned.

## Portable Zip Asset

GitHub releases should also include a portable zip for users who do not want an
installer or do not have administrator rights. The zip should be named:

```text
Axiom-<version>-win-x64.zip
```

Recommended contents:

- `Axiom.exe`.
- `axiomc.exe`.
- `backends\7zip\` for bundled read-only 7z/RAR/ISO/CAB support.
- `README.md`.
- `CLI_GUIDE.md`.
- `ARCHITECTURE.md`.
- `FORMAT.md`.
- `LICENSE`.
- `docs\`.

Build the zip from the same Release output and docs used by the installer so
both release assets describe the same version.

## Installed Files

The installer writes:

- `Axiom.exe`.
- `axiomc.exe`.
- `README.md`.
- `CLI_GUIDE.md`.
- `ARCHITECTURE.md`.
- `FORMAT.md`.
- `LICENSE`.
- `docs\`.

It also creates a Start Menu shortcut, offers an optional desktop shortcut, and
registers `Axiom.exe` and `axiomc.exe` under Windows App Paths.

## Update, Reinstall, and Remove

The installer keeps a stable Inno Setup `AppId`, so later packages update the
same installation in place and reuse the previous installation directory.

When Setup detects an existing Axiom install, it shows a maintenance choice:

- Update from an older installed version to the package version.
- Repair/reinstall when the installed version matches the package version.
- Refuse to install over a newer installed version.
- Remove Axiom by launching the existing uninstaller.

Setup uses Windows Restart Manager through `CloseApplications=yes` so in-use
Axiom binaries can be closed before files are replaced.

## File Associations

The installer does not force Axiom as the `.axar` default. Use Axiom's Settings
dialog to register archive file associations and Explorer context menu actions.
