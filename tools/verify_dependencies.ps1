param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = "Stop"

$rootPath = (Resolve-Path -LiteralPath $Root).Path
$lockPath = Join-Path $rootPath "dependencies.lock.json"
if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
    throw "Dependency lockfile was not found: $lockPath"
}

$lock = Get-Content -Raw -LiteralPath $lockPath | ConvertFrom-Json
if ($lock.schema -ne 1) {
    throw "Unsupported dependency lock schema: $($lock.schema)"
}

$failures = [System.Collections.Generic.List[string]]::new()
$checked = 0

foreach ($dependencyProperty in $lock.dependencies.psobject.Properties) {
    $name = $dependencyProperty.Name
    $dependency = $dependencyProperty.Value
    if ([string]::IsNullOrWhiteSpace($dependency.repository)) {
        $failures.Add("${name}: repository is missing")
    }
    if ([string]::IsNullOrWhiteSpace($dependency.commit) -or
        $dependency.commit -notmatch '^[0-9a-fA-F]{40}$') {
        $failures.Add("${name}: commit must be a full 40-character SHA-1")
    }
    if ([string]::IsNullOrWhiteSpace($dependency.path)) {
        $failures.Add("${name}: local path is missing")
        continue
    }

    $basePath = Join-Path $rootPath $dependency.path
    if (-not $dependency.artifacts) {
        continue
    }
    if (-not (Test-Path -LiteralPath $basePath -PathType Container)) {
        $failures.Add("${name}: artifact base path was not found: $basePath")
        continue
    }

    foreach ($artifactProperty in $dependency.artifacts.psobject.Properties) {
        $relativePath = $artifactProperty.Name
        $expectedHash = "$($artifactProperty.Value)".ToUpperInvariant()
        if ($expectedHash -notmatch '^[0-9A-F]{64}$') {
            $failures.Add("${name}/${relativePath}: invalid SHA-256 in lockfile")
            continue
        }

        $artifactPath = Join-Path $basePath $relativePath
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
            $failures.Add("${name}/${relativePath}: file was not found: $artifactPath")
            continue
        }

        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $artifactPath).Hash.ToUpperInvariant()
        $checked++
        if ($actualHash -ne $expectedHash) {
            $failures.Add("${name}/${relativePath}: expected $expectedHash, found $actualHash")
        }
    }
}

if ($failures.Count -gt 0) {
    $message = "Third-party dependency lock verification failed:`n - " + ($failures -join "`n - ")
    throw $message
}

Write-Host "Third-party dependency lock verification passed ($checked artifact hashes checked)."
