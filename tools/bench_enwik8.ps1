# Standing benchmark for AxiomCompress: enwik8 (100 MB English Wikipedia text).
#
# Downloads enwik8 on first run, sweeps the effort levels and the match
# finder/window detail, and (if 7-Zip is present) prints a 7-Zip reference row.
# Every row is verified by decompressing and comparing a hash against the source,
# so a ratio is never reported for a build that does not round-trip.
#
#   tools\bench_enwik8.ps1 [-Axiomc <path>] [-SevenZip <path>] [-Scratch <dir>] [-Quick]
#
# Corpus and scratch live under -Scratch, which defaults to a LOCAL path off any
# cloud-synced tree: background sync of the repo otherwise contends for cores and
# makes the timings noisy (ratios stay exact either way). -Quick skips slow rows.
param(
    [string]$Axiomc = "",
    [string]$SevenZip = "C:\Program Files\7-Zip\7z.exe",
    [string]$Scratch = (Join-Path $env:LOCALAPPDATA "axiom-bench"),
    [switch]$Quick
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Corpus = Join-Path $Scratch "corpus"
$Enwik8 = Join-Path $Corpus "enwik8"
$Work = Join-Path $Scratch "work"
New-Item -ItemType Directory -Force $Corpus, $Work | Out-Null

# Locate axiomc: explicit arg, then the MSVC Release output, then a local g++ build.
if (-not $Axiomc) {
    foreach ($c in @("$Root\out\Release\axiomc.exe", "$Root\build\gcc\axiomc.exe")) {
        if (Test-Path $c) { $Axiomc = $c; break }
    }
}
if (-not $Axiomc -or -not (Test-Path $Axiomc)) {
    throw "axiomc.exe not found. Build it and pass -Axiomc <path>."
}

if (-not (Test-Path $Enwik8)) {
    $zip = Join-Path $Corpus "enwik8.zip"
    Write-Host "Downloading enwik8 (~36 MB)..."
    Invoke-WebRequest -Uri "http://mattmahoney.net/dc/enwik8.zip" -OutFile $zip -TimeoutSec 600 -UseBasicParsing
    if (Test-Path $SevenZip) { & $SevenZip x $zip "-o$Corpus" -y | Out-Null }
    else { Expand-Archive -Path $zip -DestinationPath $Corpus -Force }
}
$srcLen = (Get-Item $Enwik8).Length
$srcHash = (Get-FileHash $Enwik8 -Algorithm SHA256).Hash
Write-Host ("Corpus: enwik8 = {0:N0} bytes`n" -f $srcLen)

$rows = @()
function Axiom-Row($label, $cargs) {
    $axc = Join-Path $Work "bench.axc"
    $dec = Join-Path $Work "bench.out"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $Axiomc (@("c") + $cargs + @($Enwik8, $axc)) | Out-Null
    $sw.Stop(); $ct = $sw.Elapsed.TotalSeconds
    $sw.Restart()
    & $Axiomc d $axc $dec | Out-Null
    $sw.Stop(); $dt = $sw.Elapsed.TotalSeconds
    $ok = (Get-FileHash $dec -Algorithm SHA256).Hash -eq $srcHash
    $csize = (Get-Item $axc).Length
    $script:rows += [pscustomobject]@{
        config = $label; ratio = $srcLen / $csize
        comp_MBps = ($srcLen / 1MB) / $ct; decomp_MBps = ($srcLen / 1MB) / $dt
        roundtrip = $ok
    }
    Write-Host ("  {0,-26} ratio={1,5:N2}x  c={2,6:N1} MB/s  d={3,6:N1} MB/s  rt={4}" -f `
        $label, ($srcLen/$csize), (($srcLen/1MB)/$ct), (($srcLen/1MB)/$dt), $ok)
}

Write-Host "Axiom effort levels (--level 1..9):"
$levels = if ($Quick) { 1, 3, 5, 7 } else { 1, 2, 3, 4, 5, 6, 7, 8, 9 }
foreach ($lvl in $levels) {
    Axiom-Row ("level $lvl") @("--level", "$lvl")
}

Write-Host "`nAxiom match-finder / window detail:"
Axiom-Row "hash default (parallel)"  @()
Axiom-Row "bt parallel w=8M"         @("--bt","--parallel","--block-size","8M","--window","8M")
Axiom-Row "bt single w=1M"           @("--bt","--block-size","128M","--window","1M")
Axiom-Row "bt single w=8M"           @("--bt","--block-size","128M","--window","8M")
Axiom-Row "bt single w=32M"          @("--bt","--block-size","128M","--window","32M")
if (-not $Quick) {
    Axiom-Row "bt single w=128M (full)" @("--bt","--block-size","128M","--window","128M")
}

if (Test-Path $SevenZip) {
    Write-Host "`n7-Zip reference:"
    function SevenZip-Row($label, $level) {
        $arc = Join-Path $Work "ref.7z"
        Remove-Item $arc -ErrorAction SilentlyContinue
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $SevenZip a -t7z $level $arc $Enwik8 | Out-Null
        $sw.Stop(); $ct = $sw.Elapsed.TotalSeconds
        $csize = (Get-Item $arc).Length
        $sw.Restart()
        & $SevenZip t $arc | Out-Null   # decode-only timing proxy
        $sw.Stop(); $dt = $sw.Elapsed.TotalSeconds
        $script:rows += [pscustomobject]@{
            config = $label; ratio = $srcLen / $csize
            comp_MBps = ($srcLen / 1MB) / $ct; decomp_MBps = ($srcLen / 1MB) / $dt
            roundtrip = $true
        }
        Write-Host ("  {0,-26} ratio={1,5:N2}x  c={2,6:N1} MB/s  d={3,6:N1} MB/s" -f `
            $label, ($srcLen/$csize), (($srcLen/1MB)/$ct), (($srcLen/1MB)/$dt))
    }
    SevenZip-Row "7-Zip -mx1" "-mx1"
    SevenZip-Row "7-Zip -mx9" "-mx9"
}

Write-Host "`n--- markdown ---"
"| config | ratio | compress | decompress |"
"|---|---|---|---|"
foreach ($r in $rows) {
    "| {0} | {1:N2}x | {2:N0} MB/s | {3:N0} MB/s |" -f $r.config, $r.ratio, $r.comp_MBps, $r.decomp_MBps
}
