# Installer

Axiom uses Inno Setup 6 for the Windows installer, matching NativePad's release
packaging model.

The installer uses Inno Setup's dynamic modern wizard style, so Setup and
Uninstall follow the user's Windows light/dark app mode.

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

The script builds Release x64, runs `out\Release\axiom_roundtrip.exe`, reads the
post-build version from `src\gui\axiom_gui.rc`, and writes:

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
