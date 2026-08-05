[CmdletBinding()]
param(
    [string]$Version,
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateSet("x64")]
    [string]$Platform = "x64",
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [string]$ThemeRoot,
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"
$script:RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$explicitVersion = -not [string]::IsNullOrWhiteSpace($Version)

if ([string]::IsNullOrWhiteSpace($ThemeRoot)) {
    $ThemeRoot = Join-Path (Split-Path $script:RepoRoot -Parent) "Wimukthi.Win32Theme"
}
if (-not (Test-Path -LiteralPath (Join-Path $ThemeRoot "Wimukthi.Win32Theme.props"))) {
    throw "Wimukthi.Win32Theme was not found at '$ThemeRoot'. Pass -ThemeRoot to override."
}
$ThemeRoot = (Resolve-Path -LiteralPath $ThemeRoot).Path

function Resolve-ToolPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CommandName,
        [string[]]$CandidatePaths
    )

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    foreach ($path in $CandidatePaths) {
        if ($path -and (Test-Path -LiteralPath $path)) {
            return $path
        }
    }

    return $null
}

function Find-MSBuild {
    $candidate = Resolve-ToolPath -CommandName "msbuild.exe" -CandidatePaths @(
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    )

    if ($candidate) {
        return $candidate
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($found) {
            return $found
        }
    }

    throw "MSBuild.exe was not found. Install Visual Studio 2026 or run this script from a Developer PowerShell."
}

function Find-InnoCompiler {
    $candidate = Resolve-ToolPath -CommandName "ISCC.exe" -CandidatePaths @(
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
    )

    if ($candidate) {
        return $candidate
    }

    throw "ISCC.exe was not found. Install Inno Setup 6, for example: winget install --id JRSoftware.InnoSetup --exact"
}

function Get-ResourceVersion {
    $resourceFile = Join-Path $script:RepoRoot "src\gui\axiom_gui.rc"
    $versionLine = Get-Content -LiteralPath $resourceFile | Where-Object {
        $_ -match 'VALUE\s+"ProductVersion",\s+"([^"]+)"'
    } | Select-Object -First 1

    if ($versionLine -and $versionLine -match 'VALUE\s+"ProductVersion",\s+"([^"]+)"') {
        return $Matches[1]
    }

    throw "Could not read ProductVersion from src\gui\axiom_gui.rc."
}

function Invoke-CheckedProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE."
    }
}

function Copy-BundledBackends {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    $backendSource = Join-Path $script:RepoRoot "third_party\7zip\win-x64"
    if (-not (Test-Path -LiteralPath $backendSource)) {
        throw "Bundled 7-Zip backend was not found: $backendSource"
    }
    $backendDest = Join-Path $script:RepoRoot "out\$Configuration\backends\7zip"
    if (Test-Path -LiteralPath $backendDest) {
        Remove-Item -LiteralPath $backendDest -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $backendDest | Out-Null
    foreach ($name in @("7z.dll", "License.txt", "readme.txt")) {
        Copy-Item -LiteralPath (Join-Path $backendSource $name) -Destination $backendDest -Force
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $script:RepoRoot "installer\output"
}

$solution = Join-Path $script:RepoRoot "AxiomCompress.sln"
$testExe = Join-Path $script:RepoRoot "out\$Configuration\axiom_roundtrip.exe"
$appExe = Join-Path $script:RepoRoot "out\$Configuration\Axiom.exe"
$cliExe = Join-Path $script:RepoRoot "out\$Configuration\axiomc.exe"
$sfxModule = Join-Path $script:RepoRoot "out\$Configuration\AxiomSfx.bin"
$sfxMiniModule = Join-Path $script:RepoRoot "out\$Configuration\AxiomSfxMini.bin"
$issFile = Join-Path $script:RepoRoot "installer\Axiom.iss"
$msbuild = Find-MSBuild
$iscc = Find-InnoCompiler

if (-not $SkipBuild) {
    Invoke-CheckedProcess -FilePath $msbuild -Arguments @(
        $solution,
        "/p:Configuration=$Configuration",
        "/p:Platform=$Platform",
        "/p:WimukthiWin32ThemeRoot=$ThemeRoot",
        "/m"
    )
}

Copy-BundledBackends -Configuration $Configuration

if (-not $SkipTests) {
    if (-not (Test-Path -LiteralPath $testExe)) {
        throw "Test binary was not found: $testExe"
    }

    Invoke-CheckedProcess -FilePath $testExe -Arguments @()
}

if (-not (Test-Path -LiteralPath $appExe)) {
    throw "Application binary was not found: $appExe"
}

if (-not (Test-Path -LiteralPath $cliExe)) {
    throw "CLI binary was not found: $cliExe"
}

if (-not (Test-Path -LiteralPath $sfxModule)) {
    throw "SFX module was not found: $sfxModule"
}

if (-not (Test-Path -LiteralPath $sfxMiniModule)) {
    throw "Mini SFX module was not found: $sfxMiniModule"
}

$resourceVersion = Get-ResourceVersion
if (-not $explicitVersion) {
    $Version = $resourceVersion
} elseif ($Version -ne $resourceVersion) {
    throw "Requested installer version $Version does not match src\gui\axiom_gui.rc version $resourceVersion."
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

Invoke-CheckedProcess -FilePath $iscc -Arguments @(
    "/DAppVersion=$Version",
    "/DConfiguration=$Configuration",
    "/DPlatform=$Platform",
    "/DThemeRoot=$ThemeRoot",
    "/O$OutputDir",
    $issFile
)

$installer = Join-Path $OutputDir "AxiomSetup-$Version-win-x64.exe"
if (-not (Test-Path -LiteralPath $installer)) {
    throw "Installer was not produced: $installer"
}

Write-Host "Installer created: $installer"
