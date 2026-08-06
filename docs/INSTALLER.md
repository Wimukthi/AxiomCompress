# Building the release packages

Every Axiom release ships two Windows x64 downloads: an Inno Setup installer
and a portable zip. Both are built from the same Release binaries and the same
documentation, so they always describe the same version.

## What you need

- Visual Studio Release x64 build tools.
- Inno Setup 6: `winget install --id JRSoftware.InnoSetup --exact`
- The [`Wimukthi.Win32Theme`](https://github.com/Wimukthi/Wimukthi.Win32Theme)
  repository checked out beside `AxiomCompress`, or its path passed with
  `-ThemeRoot`.

## Building the installer

```powershell
.\installer\build-installer.ps1
```

That builds Release x64, runs the Release round-trip test, reads the version
from `src\gui\axiom_gui.rc`, and writes:

```text
installer\output\AxiomSetup-<version>-win-x64.exe
```

For a tagged release, where the version in the resource file has already been
pinned, package the binaries you've already built and tested:

```powershell
.\tools\test_msvc.ps1 -Configuration Release -AutoIncrementVersion:$false
.\installer\build-installer.ps1 -SkipBuild -SkipTests -Version <version>
```

If you pass `-Version`, it must match what's currently in
`src\gui\axiom_gui.rc`. That check is what keeps the installer filename, the
release tag, the Windows file properties, the About dialog, and the uninstall
entry all saying the same thing.

## What's in the package

Both downloads carry the same payload:

| Item | Notes |
|---|---|
| `Axiom.exe` | The Windows app |
| `axiomc.exe` | The command-line tool |
| `AxiomSfx.bin` | Full-window self-extractor runtime; must stay beside the executables |
| `AxiomSfxMini.bin` | Console-only self-extractor runtime; needed by `--stub mini` |
| `backends\7zip\` | The bundled read-only 7z / RAR / ISO / CAB engine |
| `README.md`, `CHANGELOG.md`, `CLI_GUIDE.md`, `ARCHITECTURE.md`, `FORMAT.md` | User and developer documentation |
| `LICENSE`, `THIRD_PARTY_NOTICES.md` | Licensing |
| `licenses\` | Zstandard BSD, the LZMA SDK public-domain notice, and the Wimukthi.Win32Theme and Darkmodelib licenses |
| `licenses\source\darkmodelib\` | The complete corresponding Darkmodelib source, as MPL-2.0 requires |
| `docs\` | The rest of the documentation set |

`AxiomSfx.bin` and `AxiomSfxMini.bin` are not programs. They are read only when
someone creates a self-extracting archive, and both must stay beside
`Axiom.exe` and `axiomc.exe` — the CLI picks between them with
`--stub full|mini`. Ship one without the other and that tier silently stops
working.

The portable zip is named `Axiom-<version>-win-x64.zip` and is produced by the
`release-package` workflow. It exists for people who don't want an installer,
or don't have administrator rights.

## How the installer behaves

It uses Inno Setup's dynamic modern wizard style, so Setup and Uninstall follow
whatever light or dark app mode the user has chosen in Windows.

It creates a Start Menu shortcut, offers an optional desktop shortcut, and
registers `Axiom.exe` and `axiomc.exe` under Windows App Paths so they can be
launched by name.

Because the Inno Setup `AppId` is stable, a later package updates the same
installation in place and reuses the previous directory. When Setup finds an
existing install it offers a maintenance choice:

- Update from an older installed version to this one.
- Repair or reinstall, when the versions match.
- Refuse to install over a newer installed version.
- Remove Axiom, by launching the existing uninstaller.

`CloseApplications=yes` engages the Windows Restart Manager, so Axiom binaries
that are in use can be closed before files are replaced.

## File associations

The installer does **not** make Axiom the default handler for `.axar` or
anything else. File associations and the Explorer submenu are registered per
user, from Axiom's own Settings dialog, on the Integration page.

That's a deliberate choice: a per-machine installer shouldn't reach into a
user's file associations on their behalf.

## Release checklist

The full version-pinning and tagging procedure is in
[VERSIONING.md](VERSIONING.md).
