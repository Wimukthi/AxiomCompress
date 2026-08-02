param(
    [string]$BuildRoot,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [int]$MaximumMiniBytes = 1MB,

    [int]$MinimumReductionPercent = 20
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

$fullPath = Join-Path $buildOutput "AxiomSfx.bin"
$miniPath = Join-Path $buildOutput "AxiomSfxMini.bin"
foreach ($required in @($fullPath, $miniPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required SFX binary was not found: $required"
    }
}

$full = Get-Item -LiteralPath $fullPath
$mini = Get-Item -LiteralPath $miniPath
if ($full.Length -le 0) {
    throw "Full SFX binary is empty: $fullPath"
}

$reductionPercent = (1.0 - ($mini.Length / [double]$full.Length)) * 100.0
if ($mini.Length -ge $full.Length) {
    throw "Mini SFX must be smaller than the full stub ($($mini.Length) >= $($full.Length))"
}
if ($mini.Length -gt $MaximumMiniBytes) {
    throw "Mini SFX exceeds the footprint budget ($($mini.Length) > $MaximumMiniBytes bytes)"
}
if ($reductionPercent -lt $MinimumReductionPercent) {
    throw "Mini SFX reduction is below the budget ({0:N1}% < {1}%)" -f $reductionPercent, $MinimumReductionPercent
}

Write-Host ("SFX footprint regression passed: full={0:N0} bytes, mini={1:N0} bytes, reduction={2:N1}% (budget <= {3:N0} bytes and >= {4}% reduction)" -f `
    $full.Length, $mini.Length, $reductionPercent, $MaximumMiniBytes, $MinimumReductionPercent)
