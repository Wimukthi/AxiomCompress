# Bundled 7-Zip library backend

Axiom loads the 64-bit 7-Zip engine DLL directly so read-only support for 7z,
RAR/RAR5, ISO/UDF, and CAB works out of the box on Windows without launching a
console helper process or requiring 7-Zip to be installed.

Files shipped with Axiom:

- `win-x64/7z.dll`
- `win-x64/License.txt`
- `win-x64/readme.txt`

`win-x64/7z.exe` remains in the source tree only as a test-fixture generator
for interoperability tests. It is not included in Axiom installers or portable
packages.

The bundled copy is 7-Zip 26.00 for Windows x64.

7-Zip is copyright (C) 1999-2026 Igor Pavlov. See `win-x64/License.txt` for
the full license notice. In summary, 7-Zip is mainly GNU LGPL, with BSD-licensed
parts and the upstream unRAR restriction for some RAR code.

Upstream project: https://www.7-zip.org/
