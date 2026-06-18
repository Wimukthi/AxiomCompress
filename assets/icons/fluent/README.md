# Fluent UI System Icons subset

These SVG files come from Microsoft's
[Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons)
repository at commit `261a8ac124d677469412a44faf3f49e2e5eba438`.

The files were renamed for Axiom's command vocabulary. Their original names are:

| Axiom file | Upstream icon |
| --- | --- |
| `archive.svg` | `ic_fluent_archive_24_regular.svg` |
| `extract.svg` | `ic_fluent_arrow_download_24_regular.svg` |
| `test.svg` | `ic_fluent_shield_checkmark_24_regular.svg` |
| `view.svg` | `ic_fluent_eye_24_regular.svg` |
| `delete.svg` | `ic_fluent_delete_24_regular.svg` |
| `info.svg` | `ic_fluent_info_24_regular.svg` |
| `settings.svg` | `ic_fluent_settings_24_regular.svg` |
| `open.svg` | `ic_fluent_folder_open_24_regular.svg` |
| `back.svg` | `ic_fluent_arrow_left_24_regular.svg` |
| `forward.svg` | `ic_fluent_arrow_right_24_regular.svg` |
| `up.svg` | `ic_fluent_arrow_up_24_regular.svg` |
| `refresh.svg` | `ic_fluent_arrow_clockwise_24_regular.svg` |
| `pause.svg` | `ic_fluent_pause_24_regular.svg` |
| `resume.svg` | `ic_fluent_play_24_regular.svg` |
| `cancel.svg` | `ic_fluent_dismiss_24_regular.svg` |

The generated alpha masks in `src/gui/toolbar_icon_masks.inc` preserve the
antialiasing from these 24-pixel SVG masters. Axiom tints those masks at runtime
to match the active theme and disabled state.

See `LICENSE` in this directory for the upstream MIT license.
