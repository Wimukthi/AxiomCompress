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
$hashMode = "$($lock.artifact_hash_mode)"
if ($hashMode -ne "canonical-text-lf") {
    throw "Unsupported dependency artifact hash mode: $hashMode"
}

# Text files are normalized to LF before hashing so the lock remains stable
# when actions/checkout uses different line-ending defaults on each runner OS.
$binaryExtensions = @(
    ".a", ".bmp", ".dylib", ".dll", ".exe", ".gif", ".ico", ".jpeg", ".jpg",
    ".lib", ".o", ".obj", ".png", ".so", ".zip", ".7z"
)

function Get-ArtifactHash {
    param([Parameter(Mandatory)][string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    if ($binaryExtensions -notcontains $extension) {
        $canonical = [System.Collections.Generic.List[byte]]::new()
        for ($index = 0; $index -lt $bytes.Length; $index++) {
            if ($bytes[$index] -eq 13) {
                if ($index + 1 -lt $bytes.Length -and $bytes[$index + 1] -eq 10) {
                    $index++
                }
                $canonical.Add([byte]10)
            } else {
                $canonical.Add($bytes[$index])
            }
        }
        $bytes = $canonical.ToArray()
    }

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return (($sha256.ComputeHash($bytes) | ForEach-Object { $_.ToString("X2") }) -join "")
    } finally {
        $sha256.Dispose()
    }
}

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

        $actualHash = Get-ArtifactHash -Path $artifactPath
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
