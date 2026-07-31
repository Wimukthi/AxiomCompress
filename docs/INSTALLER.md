# Release packaging

Axiom ships two Windows x64 assets per release: an Inno Setup 6 installer and a
portable zip. Both are built from the same Release binaries and the same docs,
so they always describe the same version.

## Requirements

- Visual Studio Release x64 build tools.
- Inno Setup 6: `winget install --id JRSoftware.InnoSetup --exact`
- The [`Wimukthi.Win32Theme`](https://github.com/Wimukthi/Wimukthi.Win32Theme)
  repository beside `AxiomCompress`, or a path passed with `-ThemeRoot`.

## Building the installer

```powershell
.\installer\build-installer.ps1
```

The script builds Release x64, runs the Release round-trip test, reads the
version from `src\gui\axiom_gui.rc`, and writes:

```text
installer\output\AxiomSetup-<version>-win-x64.exe
```

To package binaries that are already built — the normal path for a tagged
release, where the resource version has been pinned:

```powershell
.\tools\test_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
.\installer\build-installer.ps1 -SkipBuild -SkipTests -Version <version>
```

If `-Version` is supplied it must match the current `src\gui\axiom_gui.rc`
version. That check keeps the installer name, release tag, Windows file
properties, About dialog, and uninstall metadata aligned.

## Package contents

Both assets carry the same payload:

| Item | Notes |
|---|---|
| `Axiom.exe` | GUI |
| `axiomc.exe` | CLI |
| `AxiomSfx.bin` | SFX runtime; must stay beside the executables |
| `backends\7zip\` | Bundled read-only 7z/RAR/ISO/CAB engine |
| `README.md`, `CHANGELOG.md`, `CLI_GUIDE.md`, `ARCHITECTURE.md`, `FORMAT.md` | User and developer docs |
| `LICENSE`, `THIRD_PARTY_NOTICES.md` | Licensing |
| `licenses\` | Zstandard BSD, LZMA SDK public-domain notice, Wimukthi.Win32Theme and Darkmodelib licenses |
| `licenses\source\darkmodelib\` | Complete corresponding Darkmodelib source, as MPL-2.0 requires |
| `docs\` | The rest of the documentation set |

`AxiomSfx.bin` is not a launchable program. It is read only when an SFX archive
is created, and it must remain beside `Axiom.exe` and `axiomc.exe`.

The portable zip is named `Axiom-<version>-win-x64.zip` and is produced by the
`release-package` workflow. It exists for users who do not want an installer or
do not have administrator rights.

## Installer behavior

The installer uses Inno Setup's dynamic modern wizard style, so Setup and
Uninstall follow the user's Windows light/dark app mode.

It creates a Start Menu shortcut, offers an optional desktop shortcut, and
registers `Axiom.exe` and `axiomc.exe` under Windows App Paths.

A stable Inno Setup `AppId` means later packages update the same installation
in place and reuse the previous directory. When Setup detects an existing
install it offers a maintenance choice:

- Update from an older installed version to the package version.
- Repair or reinstall when the versions match.
- Refuse to install over a newer installed version.
- Remove Axiom by launching the existing uninstaller.

`CloseApplications=yes` engages Windows Restart Manager, so in-use Axiom
binaries can be closed before files are replaced.

## File associations

The installer does **not** force Axiom as the `.axar` default. File
associations and the Explorer Axiom submenu are registered per user from
Axiom's own Settings dialog, on the Integration page.

## Release checklist

The full version-pinning and tagging procedure is in
[VERSIONING.md](VERSIONING.md).
