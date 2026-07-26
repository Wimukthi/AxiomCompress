# Third-party notices

AxiomCompress is distributed under the GNU General Public License version 3.
The following components retain their own licenses.

## Wimukthi.Win32Theme

Axiom's Windows theme integration uses `Wimukthi.Win32Theme`, an MIT-licensed
C++20 facade maintained alongside this repository:

- <https://github.com/Wimukthi/Wimukthi.Win32Theme>
- pinned commit: `f39163076210144c492f79c82979218a10553bfb`

Binary packages include its MIT license under
`licenses/Wimukthi.Win32Theme-LICENSE.txt`.

## Darkmodelib

`Wimukthi.Win32Theme` vendors Darkmodelib 0.75.0 from:

- <https://github.com/ozone10/win32-darkmodelib>
- pinned commit: `fa99647299c4edb3cf662bc14f19b5451090723e`

Darkmodelib is primarily licensed under the Mozilla Public License 2.0 and
contains files under the MIT license and other notices documented in its source
tree. Binary packages include its license files and the complete corresponding
vendored source under `licenses/source/darkmodelib`.

## Other components

The remaining bundled components keep their notices in `src/third_party`,
`third_party`, and the installed `backends` directory. See the list in
`README.md` for their roles and licenses.
