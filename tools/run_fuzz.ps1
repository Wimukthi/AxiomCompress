# Runs the libFuzzer targets for a bounded time under AddressSanitizer.
# Builds a small seed corpus from a real archive/stream so coverage ramps fast.

param(
    [int]$Seconds = 60,
    [ValidateSet("fuzz_decompress", "fuzz_archive", "all")]
    [string]$Target = "all"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $Root "build\fuzz"

# The ASan/libFuzzer runtime DLL ships beside cl.exe; put it on PATH so the
# instrumented executables can load it outside a developer shell.
function Find-VisualStudioRoot {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -property installationPath
        if ($install) { return $install }
    }
    return "C:\Program Files\Microsoft Visual Studio"
}

$clangRt = Get-ChildItem (Find-VisualStudioRoot) -Recurse `
    -Filter "clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if ($clangRt) { $env:PATH = "$($clangRt.Directory.FullName);$env:PATH" }
$env:ASAN_OPTIONS = "allocator_may_return_null=1"

# Seed corpus: a valid single-stream archive and a valid container archive.
$axiomc = Join-Path $Root "out\Release\axiomc.exe"
$seedRoot = Join-Path $outDir "seeds"
New-Item -ItemType Directory -Force -Path $seedRoot | Out-Null
if (Test-Path $axiomc) {
    $sample = Join-Path $seedRoot "sample.txt"
    ("axiom archival sample payload " * 64) | Set-Content -NoNewline $sample
    $decSeeds = Join-Path $outDir "corpus_decompress"; New-Item -ItemType Directory -Force $decSeeds | Out-Null
    $arcSeeds = Join-Path $outDir "corpus_archive"; New-Item -ItemType Directory -Force $arcSeeds | Out-Null
    & $axiomc c $sample (Join-Path $decSeeds "seed.axc") 2>$null
    & $axiomc a (Join-Path $arcSeeds "seed.axar") $sample 2>$null
}

$targets = if ($Target -eq "all") { @("fuzz_decompress", "fuzz_archive") } else { @($Target) }
$failed = $false
foreach ($t in $targets) {
    $exe = Join-Path $outDir "$t.exe"
    if (-not (Test-Path $exe)) { throw "$exe not found; run tools\build_fuzz.ps1 first." }
    $corpus = Join-Path $outDir "corpus_$($t -replace 'fuzz_','')"
    New-Item -ItemType Directory -Force -Path $corpus | Out-Null
    Write-Host "=== fuzzing $t for ${Seconds}s ==="
    # libFuzzer logs progress to stderr; don't let that abort the script.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $exe "-max_total_time=$Seconds" "-rss_limit_mb=2048" "-print_final_stats=1" $corpus 2>&1 |
        ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prevEAP
    if ($code -ne 0) { Write-Host "!! $t reported a finding (exit $code)"; $failed = $true }
}
if ($failed) { exit 1 }
Write-Host "fuzzing completed with no findings"
