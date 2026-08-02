param(
    [string]$BuildRoot,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$outputCandidates = @()
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $outputCandidates += Join-Path $repositoryRoot (Join-Path "out" $Configuration)
    $outputCandidates += Join-Path $repositoryRoot (Join-Path "build" $Configuration)
} else {
    $outputCandidates += Join-Path (Resolve-Path -LiteralPath $BuildRoot).Path $Configuration
}
$buildOutput = $outputCandidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
    Select-Object -First 1
if (-not $buildOutput) {
    throw "SFX build output was not found. Checked: $($outputCandidates -join ', ')"
}

$axiomc = Join-Path $buildOutput "axiomc.exe"
$stub = Join-Path $buildOutput "AxiomSfxMini.bin"
foreach ($required in @($axiomc, $stub)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required SFX test binary was not found: $required"
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,

        [Parameter(Mandatory)]
        [string[]]$ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Write-LegacySfx {
    param(
        [Parameter(Mandatory)]
        [string]$StubPath,

        [Parameter(Mandatory)]
        [string]$ArchivePath,

        [Parameter(Mandatory)]
        [string]$OutputPath
    )

    $stubBytes = [IO.File]::ReadAllBytes($StubPath)
    $archiveBytes = [IO.File]::ReadAllBytes($ArchivePath)
    $magicBytes = [Text.Encoding]::ASCII.GetBytes("AXIOMSFX")
    $lengthBytes = [BitConverter]::GetBytes([UInt64]$archiveBytes.Length)
    $legacyBytes = New-Object byte[] ($stubBytes.Length + $archiveBytes.Length + 16)
    [Array]::Copy($stubBytes, 0, $legacyBytes, 0, $stubBytes.Length)
    [Array]::Copy($archiveBytes, 0, $legacyBytes, $stubBytes.Length, $archiveBytes.Length)
    [Array]::Copy($magicBytes, 0, $legacyBytes, $stubBytes.Length + $archiveBytes.Length, 8)
    [Array]::Copy($lengthBytes, 0, $legacyBytes, $stubBytes.Length + $archiveBytes.Length + 8, 8)
    [IO.File]::WriteAllBytes($OutputPath, $legacyBytes)
}

function Test-SfxPayload {
    param(
        [Parameter(Mandatory)]
        [string]$SfxPath,

        [Parameter(Mandatory)]
        [string]$Destination,

        [Parameter(Mandatory)]
        [string]$ExpectedContent
    )

    Invoke-Checked $SfxPath @("--test")
    $listing = @(& $SfxPath "--list" 2>&1)
    if ($LASTEXITCODE -ne 0 -or $listing.Count -eq 0) {
        throw "SFX listing failed or returned no output: $SfxPath"
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Invoke-Checked $SfxPath @("--very-silent", "--no-run", "--output", $Destination)

    $files = @(Get-ChildItem -LiteralPath $Destination -File -Recurse)
    if ($files.Count -ne 1) {
        throw "Expected one extracted file from $SfxPath, found $($files.Count)"
    }
    $actual = [IO.File]::ReadAllText($files[0].FullName)
    if ($actual -ne $ExpectedContent) {
        throw "Extracted content did not match for $SfxPath"
    }
}

$tempRoot = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [IO.Path]::GetTempPath()
} else {
    $env:RUNNER_TEMP
}
$temp = Join-Path $tempRoot ("axiom-sfx-runtime-" + [guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    $inputFile = Join-Path $temp "payload.txt"
    $archive = Join-Path $temp "payload.axar"
    $zip = Join-Path $temp "payload.zip"
    $axarSfx = Join-Path $temp "payload-axar.exe"
    $zipSfx = Join-Path $temp "payload-zip.exe"
    $legacySfx = Join-Path $temp "payload-v1.exe"
    $expected = "SFX decode runtime regression`n"
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($inputFile, $expected, $utf8)

    Invoke-Checked $axiomc @("a", $archive, $inputFile)
    Invoke-Checked $axiomc @("sfx", "--stub", "mini", $archive, $axarSfx)
    Test-SfxPayload $axarSfx (Join-Path $temp "out-axar") $expected

    Compress-Archive -LiteralPath $inputFile -DestinationPath $zip -Force
    Invoke-Checked $axiomc @("sfx", "--stub", "mini", $zip, $zipSfx)
    Test-SfxPayload $zipSfx (Join-Path $temp "out-zip") $expected

    Write-LegacySfx $stub $archive $legacySfx
    Test-SfxPayload $legacySfx (Join-Path $temp "out-v1") $expected

    Write-Host "SFX runtime compatibility passed: AXAR v2, ZIP v2, and AXAR v1"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
