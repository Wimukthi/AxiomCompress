param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [bool]$AutoIncrementVersion = $true
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot "build_msvc.ps1") -Configuration $Configuration -AutoIncrementVersion:$AutoIncrementVersion
& (Join-Path $Root "out\$Configuration\axiom_roundtrip.exe")
if ($LASTEXITCODE -ne 0) {
    throw "AxiomRoundtrip failed with exit code $LASTEXITCODE."
}
