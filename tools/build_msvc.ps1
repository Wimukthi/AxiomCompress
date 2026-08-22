param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64")]
    [string]$Platform = "x64",

    [bool]$AutoIncrementVersion = $true,

    [string]$ThemeRoot = "",

    [switch]$SkipCleanup,

    [switch]$DeepClean
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if ([string]::IsNullOrWhiteSpace($ThemeRoot)) {
    $ThemeRoot = Join-Path (Split-Path $Root -Parent) "Wimukthi.Win32Theme"
}
if (-not (Test-Path -LiteralPath (Join-Path $ThemeRoot "Wimukthi.Win32Theme.props"))) {
    throw "Wimukthi.Win32Theme was not found at '$ThemeRoot'. Pass -ThemeRoot to override."
}
$ThemeRoot = (Resolve-Path -LiteralPath $ThemeRoot).Path

if (Test-Path $VsWhere) {
    $InstallPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($InstallPath)) {
        $MsBuild = Join-Path $InstallPath "MSBuild\Current\Bin\MSBuild.exe"
    }
}

if ([string]::IsNullOrWhiteSpace($MsBuild) -or -not (Test-Path -LiteralPath $MsBuild)) {
    $MsBuild = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter MSBuild.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*\MSBuild\Current\Bin\MSBuild.exe" } |
        Select-Object -First 1 -ExpandProperty FullName
}

if ([string]::IsNullOrWhiteSpace($MsBuild) -or -not (Test-Path -LiteralPath $MsBuild)) {
    throw "MSBuild.exe was not found."
}

& $MsBuild (Join-Path $Root "AxiomCompress.sln") /m /p:Configuration=$Configuration /p:Platform=$Platform /p:AutoIncrementVersion=$AutoIncrementVersion "/p:WimukthiWin32ThemeRoot=$ThemeRoot"
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

$BundledBackend = Join-Path $Root "third_party\7zip\win-x64"
if (Test-Path -LiteralPath $BundledBackend) {
    $BackendOut = Join-Path $Root "out\$Configuration\backends\7zip"
    if (Test-Path -LiteralPath $BackendOut) {
        Remove-Item -LiteralPath $BackendOut -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $BackendOut | Out-Null
    Copy-Item -LiteralPath (Join-Path $BundledBackend "7z.dll") -Destination $BackendOut -Force
    Copy-Item -LiteralPath (Join-Path $BundledBackend "License.txt") -Destination $BackendOut -Force
    Copy-Item -LiteralPath (Join-Path $BundledBackend "readme.txt") -Destination $BackendOut -Force
}

# ---- post-build cleanup -------------------------------------------------------

# A build leaves more behind than it ships. Incremental link and LTCG leave
# .ilk/.iobj/.ipdb beside the binaries, an export file appears wherever a target
# exports symbols, and the compiler occasionally drops scratch (vc###.pdb, .obj,
# .idb) into the working directory instead of IntDir, where nothing ever collects
# it. Everything shippable survives: the executables, the SFX runtime modules,
# the bundled backend, and the .pdb files that make a release crash readable.
# Pass -SkipCleanup to leave the tree exactly as the build left it, or -DeepClean
# to also discard the intermediate object tree (which forces a full rebuild next
# time, so it is not the default).
if (-not $SkipCleanup) {
    $removedCount = 0
    $freedBytes = 0

    function Remove-Artifact {
        param([System.IO.FileInfo]$File)
        $size = $File.Length
        Remove-Item -LiteralPath $File.FullName -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path -LiteralPath $File.FullName)) {
            $script:removedCount++
            $script:freedBytes += $size
        }
    }

    $OutDir = Join-Path $Root "out\$Configuration"
    if (Test-Path -LiteralPath $OutDir) {
        # Only link byproducts. .lib is deliberately left alone: AxiomLib and
        # AxiomSfxDecodeLib are static libraries that build to this directory and
        # the executables link them, so a .lib here is an input, not a leftover.
        # Telling those apart by name does not work either — Windows compares
        # filenames case-insensitively, so axiom.lib looks like an import library
        # belonging to Axiom.exe. These executables export nothing, so the linker
        # emits no import libraries to collect in the first place.
        Get-ChildItem -LiteralPath $OutDir -File -Recurse -ErrorAction SilentlyContinue | Where-Object {
            $_.Extension -in ".ilk", ".iobj", ".ipdb", ".exp"
        } | ForEach-Object { Remove-Artifact $_ }
    }

    # Scratch that landed in the source tree rather than under build\ or out\.
    $skipPrefixes = @(
        (Join-Path $Root "build"),
        (Join-Path $Root "out"),
        (Join-Path $Root ".git")
    )
    Get-ChildItem -LiteralPath $Root -File -Recurse -Force -ErrorAction SilentlyContinue | Where-Object {
        $path = $_.FullName
        $inSkipped = $false
        foreach ($prefix in $skipPrefixes) {
            if ($path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { $inSkipped = $true; break }
        }
        (-not $inSkipped) -and (
            $_.Name -match '^vc\d+\.(pdb|idb)$' -or
            $_.Extension -in ".obj", ".idb", ".tlog", ".ilk", ".exp"
        )
    } | ForEach-Object { Remove-Artifact $_ }

    if ($DeepClean) {
        $IntermediateRoot = Join-Path $Root "build\msvc"
        if (Test-Path -LiteralPath $IntermediateRoot) {
            $intermediates = @(Get-ChildItem -LiteralPath $IntermediateRoot -File -Recurse -ErrorAction SilentlyContinue)
            foreach ($file in $intermediates) { $freedBytes += $file.Length }
            $removedCount += $intermediates.Count
            Remove-Item -LiteralPath $IntermediateRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    if ($removedCount -gt 0) {
        "Cleanup: removed {0} build artifact(s), {1:N1} MB." -f $removedCount, ($freedBytes / 1MB)
    } else {
        "Cleanup: nothing to remove."
    }
}
