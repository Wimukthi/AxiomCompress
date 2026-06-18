param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64")]
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (Test-Path $VsWhere) {
    $InstallPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($InstallPath)) {
        throw "Visual Studio with Visual C++ tools was not found."
    }

    $MsBuild = Join-Path $InstallPath "MSBuild\Current\Bin\MSBuild.exe"
} else {
    $MsBuild = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter MSBuild.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*\MSBuild\Current\Bin\MSBuild.exe" } |
        Select-Object -First 1 -ExpandProperty FullName
}

if (-not (Test-Path $MsBuild)) {
    throw "MSBuild.exe was not found."
}

& $MsBuild (Join-Path $Root "AxiomCompress.sln") /m /p:Configuration=$Configuration /p:Platform=$Platform
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}
