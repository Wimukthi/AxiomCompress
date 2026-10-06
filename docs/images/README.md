# GUI screenshot inventory

These are captures of Axiom 0.14.0.0 and its full self-extractor, using
the native Windows GUI at 96 DPI (100% scale) with the effective dark theme.
The sample paths and archive contents are disposable documentation fixtures.
The captures were made on an isolated, hidden Windows desktop.

Each image is embedded beside its corresponding instructions in
[GUI_GUIDE.md](../GUI_GUIDE.md). Compression sizes, dates, and benchmark
figures are illustrative results from the sample fixtures.

## Coverage

The set covers the main browser, menus and context menu, filter and search,
all six Add pages, all eleven Settings pages, the color and column editors,
information and estimates, extraction, progress and results, password and
validation prompts, signing, recovery and repair, snapshot naming and history,
update review, split and join, comments and locking, full SFX authoring and
recipient dialogs, About and update checks, cleanup, and shared Windows pickers.
Single-file stream workflows reuse the shared input/output file pickers.

## Remaining capture gap

The external Setup wizard is documented in the guide but has no screenshot in
this set. Its elevated window rejects `PrintWindow` from the background
capture process. It needs a future capture in an authorized session that can
access that window; no image was fabricated or substituted. Installation,
repair, and removal were not performed while capturing this guide.

## Images

| File | Surface | Size |
|---|---|---|
| [axiom-gui.png](axiom-gui.png) | The Axiom archive browser showing an open archive in dark mode | 1180 × 760 |
| [axiom-archive-menu.png](axiom-archive-menu.png) | The Archive menu with creation, maintenance, encryption, and signature commands | 294 × 617 |
| [axiom-filesystem.png](axiom-filesystem.png) | The filesystem browser showing the sample project and folder tree | 1538 × 733 |
| [axiom-filter.png](axiom-filter.png) | The floating instant filter using an extension query | 480 × 66 |
| [axiom-context-menu.png](axiom-context-menu.png) | The browser context menu for an archived text file | 269 × 587 |
| [axiom-find.png](axiom-find.png) | Find files showing text-file matches in the sample project | 1042 × 710 |
| [axiom-filesystem-info.png](axiom-filesystem-info.png) | Filesystem information after estimating compression at level 5 | 696 × 639 |
| [axiom-archive-info.png](axiom-archive-info.png) | Archive information with capabilities, storage accounting, and largest files | 1050 × 762 |
| [axiom-add-to-archive.png](axiom-add-to-archive.png) | The Add to archive dialog, showing compression settings and a live size preview | 1121 × 707 |
| [axiom-add-general.png](axiom-add-general.png) | The Add dialog General page with update mode, comment, lock, and metadata notes | 1121 × 707 |
| [axiom-add-deduplication.png](axiom-add-deduplication.png) | The Add dialog Deduplication page with chunk-size controls enabled | 1121 × 707 |
| [axiom-add-recovery.png](axiom-add-recovery.png) | The Add dialog Recovery and volumes page with a five-percent recovery record | 1121 × 707 |
| [axiom-extract.png](axiom-extract.png) | Extract archive with destination, thread count, overwrite, and timestamp choices | 540 × 290 |
| [axiom-progress.png](axiom-progress.png) | A paused compression operation showing progress and remaining time | 656 × 329 |
| [axiom-result.png](axiom-result.png) | The completion message after creating the sample archive | 446 × 167 |
| [axiom-recovery.png](axiom-recovery.png) | Recovery record percentage for an existing archive | 536 × 299 |
| [axiom-repair.png](axiom-repair.png) | The confirmation before repairing an archive | 446 × 202 |
| [axiom-add-security.png](axiom-add-security.png) | The Add dialog Security page with masked matching passwords | 1121 × 707 |
| [axiom-password.png](axiom-password.png) | The password prompt shown before opening an encrypted directory | 436 × 217 |
| [axiom-keygen-picker.png](axiom-keygen-picker.png) | The Save dialog for a newly generated secret signing key | 960 × 540 |
| [axiom-signing-picker.png](axiom-signing-picker.png) | The file picker for signing an existing archive | 960 × 540 |
| [axiom-signature.png](axiom-signature.png) | Verification reporting a valid signature and its signer fingerprint | 446 × 172 |
| [axiom-snapshots.png](axiom-snapshots.png) | The snapshot timeline with two retained versions and the selected change list | 1096 × 739 |
| [axiom-snapshot-name.png](axiom-snapshot-name.png) | The snapshot name dialog before capturing a new version | 596 × 227 |
| [axiom-add-sfx.png](axiom-add-sfx.png) | The Add dialog SFX page with extractor behavior and sample license text | 1121 × 707 |
| [axiom-sfx-convert.png](axiom-sfx-convert.png) | The confirmation for converting an existing archive into a self-extractor | 446 × 172 |
| [axiom-sfx-license.png](axiom-sfx-license.png) | A full self-extractor requiring acceptance of its sample license | 636 × 559 |
| [axiom-sfx-recipient.png](axiom-sfx-recipient.png) | The full self-extractor destination and extraction options | 696 × 509 |
| [axiom-update-preview.png](axiom-update-preview.png) | The read-only archive update plan with search, action filter, and size impact | 1251 × 689 |
| [axiom-split.png](axiom-split.png) | The split-volume size dialog for an existing archive | 536 × 299 |
| [axiom-join-picker.png](axiom-join-picker.png) | The Join picker filtered to Axiom data and recovery volumes | 960 × 540 |
| [axiom-comment.png](axiom-comment.png) | The multiline archive comment editor | 639 × 422 |
| [axiom-lock.png](axiom-lock.png) | The confirmation before permanently locking an archive | 446 × 187 |
| [axiom-settings.png](axiom-settings.png) | The Axiom settings dialog, showing the General page | 938 × 667 |
| [axiom-color-picker.png](axiom-color-picker.png) | The custom accent picker with palette, hue strip, preview, and hexadecimal input | 596 × 389 |
| [axiom-settings-compression.png](axiom-settings-compression.png) | Settings Compression with defaults for future Add operations | 938 × 760 |
| [axiom-settings-paths.png](axiom-settings-paths.png) | Settings Paths with archive, extraction, and temporary-folder policies | 938 × 667 |
| [axiom-settings-filelist.png](axiom-settings-filelist.png) | Settings File list with browsing behavior and the column editor button | 938 × 667 |
| [axiom-columns.png](axiom-columns.png) | The column editor with visibility, width, order, and default controls | 776 × 559 |
| [axiom-settings-viewer.png](axiom-settings-viewer.png) | Settings Viewer with application paths, warnings, and temporary-file retention | 938 × 667 |
| [axiom-settings-security.png](axiom-settings-security.png) | Settings Security with password reuse, verification, trusted keys, and plaintext wiping | 938 × 667 |
| [axiom-settings-integration.png](axiom-settings-integration.png) | Settings Integration with file associations and Explorer submenu commands | 938 × 667 |
| [axiom-settings-updates.png](axiom-settings-updates.png) | Settings Updates with startup checking, channel, and custom HTTPS feed | 938 × 667 |
| [axiom-settings-shortcuts.png](axiom-settings-shortcuts.png) | Settings Shortcuts with command selection, chord assignment, and defaults | 938 × 667 |
| [axiom-settings-toolbar.png](axiom-settings-toolbar.png) | Settings Toolbar with label style and command visibility in fixed catalog order | 938 × 667 |
| [axiom-settings-advanced.png](axiom-settings-advanced.png) | Settings Advanced with worker priority, logging, I/O buffer, and memory limit | 938 × 667 |
| [axiom-benchmark.png](axiom-benchmark.png) | The Axiom benchmark window after a completed run | 956 × 713 |
| [axiom-update.png](axiom-update.png) | A manual update check reporting that Axiom is current | 446 × 172 |
| [axiom-about.png](axiom-about.png) | About Axiom with the release version, credits, license, and update controls | 556 × 509 |
| [axiom-source-choice.png](axiom-source-choice.png) | The source-choice prompt offering folder, files, or cancellation | 446 × 187 |
| [axiom-file-picker.png](axiom-file-picker.png) | The Windows Open dialog browsing disposable sample archives | 960 × 540 |
| [axiom-folder-picker.png](axiom-folder-picker.png) | The Windows folder picker for restoring a retained snapshot | 960 × 540 |
| [axiom-validation.png](axiom-validation.png) | A validation message explaining mismatched archive passwords | 446 × 167 |
| [axiom-cleanup.png](axiom-cleanup.png) | The confirmation before cleaning up Axiom temporary files | 446 × 187 |
