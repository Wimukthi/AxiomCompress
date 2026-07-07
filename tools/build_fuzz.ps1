# Builds the libFuzzer targets with MSVC, instrumented with AddressSanitizer.
# MSVC ships the libFuzzer runtime (clang_rt.fuzzer), so no external clang is
# required. The targets are compiled together with the library sources so ASan
# instruments the decode paths, not just the harness.

param(
    [ValidateSet("fuzz_decompress", "fuzz_archive", "all")]
    [string]$Target = "all"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -property installationPath
        if ($install) {
            $candidate = Join-Path $install "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    $fallback = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $fallback) { throw "vcvars64.bat not found; install the Visual C++ tools." }
    return $fallback
}

$vcvars = Find-VcVars
$outDir = Join-Path $Root "build\fuzz"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$libSources = @(
    "src\archive\container.cpp",
    "src\archive\container_formats.cpp",
    "src\archive\container_zip.cpp",
    "src\archive\system_provider.cpp",
    "src\codec\block.cpp",
    "src\codec\fast_lz.cpp",
    "src\codec\lz77.cpp",
    "src\codec\lz77_split.cpp",
    "src\core\archive.cpp",
    "src\core\checksum.cpp",
    "src\core\checksum_clmul.cpp",
    "src\core\cpu.cpp",
    "src\core\crypto.cpp",
    "src\core\file_meta.cpp",
    "src\core\reed_solomon.cpp",
    "src\entropy\huffman.cpp",
    "src\entropy\range.cpp",
    "src\third_party\blake3\blake3.c",
    "src\third_party\blake3\blake3_dispatch.c",
    "src\third_party\blake3\blake3_portable.c",
    "src\third_party\miniz\miniz.c",
    "src\third_party\miniz\miniz_tdef.c",
    "src\third_party\miniz\miniz_tinfl.c",
    "src\third_party\miniz\miniz_zip.c",
    "src\third_party\monocypher\monocypher.c"
) | ForEach-Object { "`"$Root\$_`"" }

# Vendored BLAKE3: portable build (SIMD backends disabled).
$blake3Defs = "/DBLAKE3_NO_AVX512 /DBLAKE3_NO_AVX2 /DBLAKE3_NO_SSE41 /DBLAKE3_NO_SSE2"

$targets = if ($Target -eq "all") { @("fuzz_decompress", "fuzz_archive") } else { @($Target) }

foreach ($t in $targets) {
    $objDir = Join-Path $outDir "$t-obj"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $flags = "/nologo /std:c++20 /O1 /Zi /EHsc /MD /fsanitize=fuzzer /fsanitize=address $blake3Defs"
    $includes = "/I `"$Root\include`" /I `"$Root\src`""
    $inputs = ($libSources -join ' ') + " `"$Root\fuzz\$t.cpp`""
    $cl = "cl $flags $includes $inputs /Fe:`"$outDir\$t.exe`" /Fo:`"$objDir\\`""
    $bat = Join-Path $outDir "build_$t.bat"
    "@echo off`r`ncall `"$vcvars`" >nul 2>&1`r`n$cl" | Set-Content -Encoding ascii $bat
    Write-Host "Building $t ..."
    # vcvars writes benign warnings to stderr; gate on the exit code, not stderr.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $log = & cmd /c "`"$bat`"" 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prevEAP
    if ($code -ne 0) {
        $log | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        throw "fuzzer build failed for $t"
    }
    Write-Host "  -> $outDir\$t.exe"
}
