[CmdletBinding()]
param(
    [string]$ResourceFile = (Join-Path $PSScriptRoot "..\src\gui\axiom_gui.rc"),
    [switch]$IncrementBuild,
    [switch]$PrintVersion
)

$ErrorActionPreference = "Stop"

function Get-VersionParts {
    param([string]$Text)

    $patterns = @(
        @{ Name = "FILEVERSION"; Pattern = '(?m)^\s*FILEVERSION\s+(?<major>\d+),(?<minor>\d+),(?<patch>\d+),(?<build>\d+)' },
        @{ Name = "PRODUCTVERSION"; Pattern = '(?m)^\s*PRODUCTVERSION\s+(?<major>\d+),(?<minor>\d+),(?<patch>\d+),(?<build>\d+)' },
        @{ Name = "FileVersion"; Pattern = 'VALUE\s+"FileVersion",\s+"(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)\.(?<build>\d+)"' },
        @{ Name = "ProductVersion"; Pattern = 'VALUE\s+"ProductVersion",\s+"(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)\.(?<build>\d+)"' }
    )

    $found = @{}
    foreach ($entry in $patterns) {
        $match = [regex]::Match($Text, $entry.Pattern)
        if (-not $match.Success) {
            throw "Could not find $($entry.Name) in $ResourceFile."
        }

        $parts = [int[]]@(
            [int]$match.Groups["major"].Value,
            [int]$match.Groups["minor"].Value,
            [int]$match.Groups["patch"].Value,
            [int]$match.Groups["build"].Value
        )

        foreach ($part in $parts) {
            if ($part -lt 0 -or $part -gt 65535) {
                throw "$($entry.Name) contains component $part, but Windows version components must be 0-65535."
            }
        }

        $found[$entry.Name] = $parts
    }

    $reference = $found["FILEVERSION"]
    $referenceText = Format-DottedVersion -Parts $reference
    foreach ($entry in $found.GetEnumerator()) {
        $currentText = Format-DottedVersion -Parts $entry.Value
        if ($currentText -ne $referenceText) {
            throw "Version fields in $ResourceFile are not aligned: FILEVERSION=$referenceText, $($entry.Key)=$currentText."
        }
    }

    return $reference
}

function Format-DottedVersion {
    param([int[]]$Parts)
    return "$($Parts[0]).$($Parts[1]).$($Parts[2]).$($Parts[3])"
}

function Format-CommaVersion {
    param([int[]]$Parts)
    return "$($Parts[0]),$($Parts[1]),$($Parts[2]),$($Parts[3])"
}

function Update-VersionText {
    param(
        [string]$Text,
        [int[]]$Parts
    )

    $dotted = Format-DottedVersion -Parts $Parts
    $comma = Format-CommaVersion -Parts $Parts

    $updated = [regex]::Replace($Text, '(?m)^(\s*FILEVERSION\s+)\d+,\d+,\d+,\d+', "`${1}$comma", 1)
    $updated = [regex]::Replace($updated, '(?m)^(\s*PRODUCTVERSION\s+)\d+,\d+,\d+,\d+', "`${1}$comma", 1)
    $updated = [regex]::Replace($updated, '(VALUE\s+"FileVersion",\s+")\d+\.\d+\.\d+\.\d+(")', "`${1}$dotted`${2}", 1)
    $updated = [regex]::Replace($updated, '(VALUE\s+"ProductVersion",\s+")\d+\.\d+\.\d+\.\d+(")', "`${1}$dotted`${2}", 1)

    return $updated
}

$resolvedResourceFile = Resolve-Path -LiteralPath $ResourceFile
$text = [System.IO.File]::ReadAllText($resolvedResourceFile)
$parts = Get-VersionParts -Text $text

if ($IncrementBuild) {
    if ($parts[3] -ge 65535) {
        throw "Build component is already 65535. Increment patch/minor/major and reset build before continuing."
    }

    $parts[3]++
    $updated = Update-VersionText -Text $text -Parts $parts

    if ($updated -eq $text) {
        throw "Version text did not change; refusing to write $resolvedResourceFile."
    }

    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($resolvedResourceFile, $updated, $utf8NoBom)
}

if ($PrintVersion -or -not $IncrementBuild) {
    Write-Output (Format-DottedVersion -Parts $parts)
}
