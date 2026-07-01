param(
    [string]$BaselineAxiomc,

    [string]$CurrentAxiomc = (Join-Path (Split-Path -Parent $PSScriptRoot) "out\Release\axiomc.exe"),

    [string]$CorpusDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "out\benchmark-corpora"),

    [string]$OutputDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "out\benchmarks\axiom-levels"),

    [int[]]$Levels = @(1, 2, 3, 4, 5, 6, 7, 8, 9),

    [string[]]$Profiles = @(),

    [ValidateRange(1, 50)]
    [int]$Repeats = 3,

    [switch]$GenerateSampleCorpora,

    [ValidateRange(1, 1024)]
    [int]$SampleSizeMiB = 8,

    [switch]$Recurse,

    [switch]$KeepOutputs
)

$ErrorActionPreference = "Stop"

function Resolve-ExistingFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function New-Directory {
    param([Parameter(Mandatory = $true)][string]$Path)

    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-SafeName {
    param([Parameter(Mandatory = $true)][string]$Name)

    $invalid = [IO.Path]::GetInvalidFileNameChars()
    $builder = [Text.StringBuilder]::new()
    foreach ($ch in $Name.ToCharArray()) {
        if ($invalid -contains $ch) {
            [void]$builder.Append("_")
        } else {
            [void]$builder.Append($ch)
        }
    }

    $safe = $builder.ToString()
    if ([string]::IsNullOrWhiteSpace($safe)) {
        return "item"
    }
    return $safe
}

function Get-Median {
    param([Parameter(Mandatory = $true)][double[]]$Values)

    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) {
        return 0.0
    }
    if (($sorted.Count % 2) -eq 1) {
        return $sorted[[int]($sorted.Count / 2)]
    }
    return ($sorted[$sorted.Count / 2 - 1] + $sorted[$sorted.Count / 2]) / 2.0
}

function Split-ProfileArguments {
    param([Parameter(Mandatory = $true)][string]$Text)

    $result = @()
    $builder = [Text.StringBuilder]::new()
    $quote = $null
    $doubleQuote = [char]34
    $singleQuote = [char]39

    foreach ($ch in $Text.ToCharArray()) {
        if ($null -ne $quote) {
            if ($ch -eq $quote) {
                $quote = $null
            } else {
                [void]$builder.Append($ch)
            }
        } elseif ($ch -eq $doubleQuote -or $ch -eq $singleQuote) {
            $quote = $ch
        } elseif ([char]::IsWhiteSpace($ch)) {
            if ($builder.Length -gt 0) {
                $result += $builder.ToString()
                [void]$builder.Clear()
            }
        } else {
            [void]$builder.Append($ch)
        }
    }

    if ($null -ne $quote) {
        throw "Unterminated quote in profile arguments: $Text"
    }
    if ($builder.Length -gt 0) {
        $result += $builder.ToString()
    }

    return $result
}

function Get-ProfileLevel {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    for ($i = 0; $i -lt $Arguments.Count; ++$i) {
        if ($Arguments[$i] -eq "--fast") {
            return 1
        }
        if ($Arguments[$i] -eq "--max") {
            return 9
        }
        if ($Arguments[$i] -eq "--level" -and $i + 1 -lt $Arguments.Count) {
            return [int]$Arguments[$i + 1]
        }
    }

    return $null
}

function ConvertTo-Profile {
    param([Parameter(Mandatory = $true)][string]$Spec)

    $separator = $Spec.IndexOf("=")
    if ($separator -le 0) {
        throw "Invalid profile '$Spec'. Expected name=arguments."
    }

    $name = $Spec.Substring(0, $separator).Trim()
    $argumentText = $Spec.Substring($separator + 1).Trim()
    if ([string]::IsNullOrWhiteSpace($name)) {
        throw "Profile name is empty in '$Spec'."
    }
    if ([string]::IsNullOrWhiteSpace($argumentText)) {
        throw "Profile '$name' has no arguments."
    }

    $arguments = @(Split-ProfileArguments $argumentText)
    if ($arguments.Count -eq 0) {
        throw "Profile '$name' has no arguments."
    }

    return [pscustomobject]@{
        Name = $name
        Level = Get-ProfileLevel $arguments
        Args = $arguments
    }
}

function Write-RepeatedCorpus {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Line,
        [Parameter(Mandatory = $true)][int]$Bytes
    )

    $pattern = [Text.Encoding]::UTF8.GetBytes($Line)
    $buffer = New-Object byte[] $Bytes
    for ($i = 0; $i -lt $buffer.Length; ++$i) {
        $buffer[$i] = $pattern[$i % $pattern.Length]
    }
    [IO.File]::WriteAllBytes($Path, $buffer)
}

function Write-MixedCorpus {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$Bytes
    )

    $pattern = [Text.Encoding]::UTF8.GetBytes(
        "Axiom compression benchmark line with repeated words, paths, numbers 0123456789, and symbols <>[]{}.`n")
    $buffer = New-Object byte[] $Bytes
    for ($i = 0; $i -lt $buffer.Length; ++$i) {
        $buffer[$i] = $pattern[$i % $pattern.Length]
    }
    for ($i = 0; $i -lt $buffer.Length; $i += 4096) {
        $value = ([int64]$i * 2654435761L) -band 0xffffffffL
        $buffer[$i] = [byte]($value -band 0xff)
        $buffer[$i + 1] = [byte](($value -shr 8) -band 0xff)
        $buffer[$i + 2] = [byte](($value -shr 16) -band 0xff)
        $buffer[$i + 3] = [byte](($value -shr 24) -band 0xff)
    }
    [IO.File]::WriteAllBytes($Path, $buffer)
}

function Write-RandomCorpus {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$Bytes
    )

    $buffer = New-Object byte[] $Bytes
    [int64]$state = 0x12345678
    for ($i = 0; $i -lt $buffer.Length; ++$i) {
        $state = (($state * 1664525L + 1013904223L) -band 0xffffffffL)
        $buffer[$i] = [byte](($state -shr 24) -band 0xff)
    }
    [IO.File]::WriteAllBytes($Path, $buffer)
}

function New-SampleCorpora {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][int]$SizeMiB
    )

    $bytes = $SizeMiB * 1MB
    New-Directory $Directory | Out-Null

    Write-RepeatedCorpus `
        -Path (Join-Path $Directory "text-$($SizeMiB)m.bin") `
        -Line (("Lorem ipsum Axiom archive compression source code documentation repeated phrase 0123456789 " * 2) + "`n") `
        -Bytes $bytes
    Write-MixedCorpus `
        -Path (Join-Path $Directory "mixed-$($SizeMiB)m.bin") `
        -Bytes $bytes
    Write-RandomCorpus `
        -Path (Join-Path $Directory "random-$($SizeMiB)m.bin") `
        -Bytes $bytes
}

function Invoke-Axiom {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    $elapsed = Measure-Command {
        & $Exe @Arguments | Out-Null
    }
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage (exit code $LASTEXITCODE)"
    }
    return $elapsed
}

$CurrentAxiomc = Resolve-ExistingFile $CurrentAxiomc "Current axiomc.exe"
$CorpusDir = New-Directory $CorpusDir
$OutputDir = New-Directory $OutputDir

if ($GenerateSampleCorpora) {
    New-SampleCorpora -Directory $CorpusDir -SizeMiB $SampleSizeMiB
}

$getFilesArgs = @{
    LiteralPath = $CorpusDir
    File = $true
}
if ($Recurse) {
    $getFilesArgs.Recurse = $true
}
$corpora = @(Get-ChildItem @getFilesArgs | Sort-Object FullName)
if ($corpora.Count -eq 0) {
    throw "No corpus files found in $CorpusDir. Pass -GenerateSampleCorpora or provide input files."
}

$profileRows = @()
if ($Profiles.Count -gt 0) {
    foreach ($profileSpec in $Profiles) {
        $profileRows += ConvertTo-Profile $profileSpec
    }
} else {
    foreach ($level in $Levels) {
        if ($level -lt 1 -or $level -gt 9) {
            throw "Invalid level: $level. Expected 1..9."
        }
        $profileRows += [pscustomobject]@{
            Name = "level$level"
            Level = $level
            Args = @("--level", [string]$level)
        }
    }
}

$duplicateProfile = $profileRows | Group-Object Name | Where-Object { $_.Count -gt 1 } | Select-Object -First 1
if ($duplicateProfile) {
    throw "Duplicate profile name: $($duplicateProfile.Name)"
}

$tools = @()
if (-not [string]::IsNullOrWhiteSpace($BaselineAxiomc)) {
    $BaselineAxiomc = Resolve-ExistingFile $BaselineAxiomc "Baseline axiomc.exe"
    $tools += [pscustomobject]@{ Name = "baseline"; Exe = $BaselineAxiomc }
}
$tools += [pscustomobject]@{ Name = "current"; Exe = $CurrentAxiomc }
$hasBaseline = $tools.Name -contains "baseline"

$rawRows = @()

foreach ($corpus in $corpora) {
    $inputHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $corpus.FullName).Hash
    $inputMiB = $corpus.Length / 1MB
    $safeCorpus = Get-SafeName $corpus.BaseName

    foreach ($profile in $profileRows) {
        $safeProfile = Get-SafeName $profile.Name

        foreach ($tool in $tools) {
            for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
                $prefix = "$($tool.Name)-$safeCorpus-$safeProfile-r$repeat"
                $archive = Join-Path $OutputDir "$prefix.axc"
                $restore = Join-Path $OutputDir "$prefix.restore"
                Remove-Item -LiteralPath $archive, $restore -Force -ErrorAction SilentlyContinue

                $compressArgs = @("c") + $profile.Args + @($corpus.FullName, $archive)
                $compress = Invoke-Axiom `
                    -Exe $tool.Exe `
                    -Arguments $compressArgs `
                    -FailureMessage "$($tool.Name) compress failed for $($corpus.Name) profile $($profile.Name) repeat $repeat"

                $archiveBytes = (Get-Item -LiteralPath $archive).Length

                $decompressArgs = @("d", $archive, $restore)
                $decompress = Invoke-Axiom `
                    -Exe $tool.Exe `
                    -Arguments $decompressArgs `
                    -FailureMessage "$($tool.Name) decompress failed for $($corpus.Name) profile $($profile.Name) repeat $repeat"

                $restoreHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $restore).Hash
                if ($restoreHash -ne $inputHash) {
                    throw "$($tool.Name) hash mismatch for $($corpus.Name) profile $($profile.Name) repeat $repeat"
                }

                $rawRows += [pscustomobject]@{
                    Tool = $tool.Name
                    Corpus = $corpus.Name
                    Profile = $profile.Name
                    Level = $profile.Level
                    Repeat = $repeat
                    InputBytes = $corpus.Length
                    ArchiveBytes = $archiveBytes
                    Ratio = [math]::Round($corpus.Length / $archiveBytes, 6)
                    CompressSeconds = [math]::Round($compress.TotalSeconds, 6)
                    DecompressSeconds = [math]::Round($decompress.TotalSeconds, 6)
                    CompressMBs = [math]::Round($inputMiB / [Math]::Max($compress.TotalSeconds, 0.001), 4)
                    DecompressMBs = [math]::Round($inputMiB / [Math]::Max($decompress.TotalSeconds, 0.001), 4)
                }

                if (-not $KeepOutputs) {
                    Remove-Item -LiteralPath $archive, $restore -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }
}

$summaryRows = @()
$deltaRows = @()
foreach ($corpus in ($rawRows.Corpus | Sort-Object -Unique)) {
    foreach ($profile in $profileRows) {
        foreach ($tool in $tools.Name) {
            $rows = @($rawRows | Where-Object {
                $_.Tool -eq $tool -and $_.Corpus -eq $corpus -and $_.Profile -eq $profile.Name
            })
            if ($rows.Count -eq 0) {
                continue
            }

            $summaryRows += [pscustomobject]@{
                Tool = $tool
                Corpus = $corpus
                Profile = $profile.Name
                Level = $profile.Level
                Repeats = $rows.Count
                ArchiveBytes = [int64](Get-Median @($rows.ArchiveBytes))
                Ratio = [math]::Round((Get-Median @($rows.Ratio)), 6)
                MedianCompressMBs = [math]::Round((Get-Median @($rows.CompressMBs)), 4)
                MedianDecompressMBs = [math]::Round((Get-Median @($rows.DecompressMBs)), 4)
            }
        }

        if ($hasBaseline) {
            $baseline = $summaryRows | Where-Object {
                $_.Tool -eq "baseline" -and $_.Corpus -eq $corpus -and $_.Profile -eq $profile.Name
            } | Select-Object -First 1
            $current = $summaryRows | Where-Object {
                $_.Tool -eq "current" -and $_.Corpus -eq $corpus -and $_.Profile -eq $profile.Name
            } | Select-Object -First 1

            if ($baseline -and $current) {
                $deltaRows += [pscustomobject]@{
                    Corpus = $corpus
                    Profile = $profile.Name
                    Level = $profile.Level
                    RatioDeltaPct = [math]::Round((($current.Ratio / $baseline.Ratio) - 1.0) * 100.0, 4)
                    CompressDeltaPct = [math]::Round((($current.MedianCompressMBs / $baseline.MedianCompressMBs) - 1.0) * 100.0, 4)
                    DecompressDeltaPct = [math]::Round((($current.MedianDecompressMBs / $baseline.MedianDecompressMBs) - 1.0) * 100.0, 4)
                    BaselineRatio = $baseline.Ratio
                    CurrentRatio = $current.Ratio
                    BaselineCompressMBs = $baseline.MedianCompressMBs
                    CurrentCompressMBs = $current.MedianCompressMBs
                    BaselineDecompressMBs = $baseline.MedianDecompressMBs
                    CurrentDecompressMBs = $current.MedianDecompressMBs
                }
            }
        }
    }
}

$rawPath = Join-Path $OutputDir "axiom-levels-raw.csv"
$summaryPath = Join-Path $OutputDir "axiom-levels-summary.csv"
$deltaPath = Join-Path $OutputDir "axiom-levels-delta.csv"

$rawRows | Export-Csv -NoTypeInformation -Path $rawPath
$summaryRows | Export-Csv -NoTypeInformation -Path $summaryPath
$deltaRows | Export-Csv -NoTypeInformation -Path $deltaPath

if ($deltaRows.Count -gt 0) {
    $deltaRows |
        Select-Object Corpus, Profile, Level, RatioDeltaPct, CompressDeltaPct, DecompressDeltaPct |
        Format-Table -AutoSize
} else {
    $summaryRows |
        Select-Object Tool, Corpus, Profile, Level, Ratio, MedianCompressMBs, MedianDecompressMBs |
        Format-Table -AutoSize
}
Write-Host ""
Write-Host "Raw results:     $rawPath"
Write-Host "Summary results: $summaryPath"
Write-Host "Delta results:   $deltaPath"
