param(
    [string]$BuildRoot = (Join-Path $PSScriptRoot "..\build"),

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$buildOutput = (Resolve-Path -LiteralPath (Join-Path $BuildRoot $Configuration)).Path
$axiomc = Join-Path $buildOutput "axiomc.exe"
$stub = Join-Path $buildOutput "AxiomSfxMini.bin"
foreach ($required in @($axiomc, $stub)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required SFX test binary was not found: $required"
    }
}

function Find-SignTool {
    $fromPath = Get-Command signtool.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty Source
    if ($fromPath) { return $fromPath }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (-not (Test-Path -LiteralPath $kitsRoot -PathType Container)) {
        throw "Windows SDK signtool.exe was not found"
    }
    $candidate = Get-ChildItem -LiteralPath $kitsRoot -Filter "signtool.exe" -File -Recurse |
        Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" } |
        Sort-Object FullName |
        Select-Object -Last 1
    if (-not $candidate) { throw "Windows SDK signtool.exe was not found" }
    return $candidate.FullName
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

function New-TestCodeSigningCertificate {
    $rsa = [System.Security.Cryptography.RSA]::Create(2048)
    try {
        $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            "CN=AxiomCompress CI SFX",
            $rsa,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $usages = [System.Security.Cryptography.OidCollection]::new()
        [void]$usages.Add([System.Security.Cryptography.Oid]::new(
            "1.3.6.1.5.5.7.3.3",
            "Code Signing"))
        $request.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new(
                $usages,
                $true))
        return $request.CreateSelfSigned(
            [DateTimeOffset]::Now.AddMinutes(-5),
            [DateTimeOffset]::Now.AddDays(1))
    } finally {
        $rsa.Dispose()
    }
}

function Assert-EmbeddedAuthenticodeSignature {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string]$ExpectedThumbprint
    )

    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if (-not $signature.SignerCertificate) {
        throw "Signed SFX did not contain an embedded signer certificate"
    }
    if ($signature.SignerCertificate.Thumbprint -ne $ExpectedThumbprint) {
        throw "Signed SFX signer certificate did not match the test certificate"
    }
    # A self-signed test certificate is intentionally not installed into any
    # trust store. Valid/NotTrusted/UnknownError all indicate that Windows
    # parsed the embedded Authenticode signature; hash or format failures do
    # not. The custom-root chain check below keeps certificate validation
    # deterministic without contacting the hosted runner's trust services.
    if ("$($signature.Status)" -notin @("Valid", "NotTrusted", "UnknownError")) {
        throw "Authenticode signature status was $($signature.Status): $($signature.StatusMessage)"
    }

    $chain = [System.Security.Cryptography.X509Certificates.X509Chain]::new()
    try {
        $trustModeProperty = $chain.ChainPolicy.GetType().GetProperty("TrustMode")
        if ($trustModeProperty) {
            $customTrustMode = [Enum]::Parse($trustModeProperty.PropertyType, "CustomRootTrust")
            $trustModeProperty.SetValue($chain.ChainPolicy, $customTrustMode)
            $customRoots = $chain.ChainPolicy.GetType().GetProperty("CustomTrustStore").GetValue($chain.ChainPolicy)
            $customRoots.Add($signature.SignerCertificate)
            if (-not $chain.Build($signature.SignerCertificate)) {
                $errors = $chain.ChainStatus | ForEach-Object { $_.StatusInformation.Trim() }
                throw "Test certificate chain did not validate: $($errors -join '; ')"
            }
        } elseif ($signature.SignerCertificate.Subject -ne $signature.SignerCertificate.Issuer) {
            throw "Test certificate was expected to be self-signed"
        }
    } finally {
        $chain.Dispose()
    }
}

$signTool = Find-SignTool
$tempRoot = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [IO.Path]::GetTempPath()
} else {
    $env:RUNNER_TEMP
}
$temp = Join-Path $tempRoot ("axiom-sfx-authenticode-" + [guid]::NewGuid().ToString("N"))
$certificate = $null
$thumbprint = $null

try {
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    $inputFile = Join-Path $temp "payload.txt"
    $archive = Join-Path $temp "payload.axar"
    $unsigned = Join-Path $temp "payload-unsigned.exe"
    $signed = Join-Path $temp "payload-signed.exe"
    $pfx = Join-Path $temp "test-signing.pfx"
    $destination = Join-Path $temp "extracted"

    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($inputFile, "Authenticode SFX regression payload`n", $utf8)
    Invoke-Checked $axiomc @("a", $archive, $inputFile)
    Invoke-Checked $axiomc @("sfx", "--stub", "mini", $archive, $unsigned)

    $certificate = New-TestCodeSigningCertificate
    $thumbprint = $certificate.Thumbprint
    $passwordText = [guid]::NewGuid().ToString("N")
    [IO.File]::WriteAllBytes(
        $pfx,
        $certificate.Export(
            [System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx,
            $passwordText))

    Copy-Item -LiteralPath $unsigned -Destination $signed
    Invoke-Checked $signTool @(
        "sign", "/fd", "SHA256", "/f", $pfx, "/p", $passwordText,
        "/d", "AxiomCompress CI SFX", $signed
    )
    Assert-EmbeddedAuthenticodeSignature -Path $signed -ExpectedThumbprint $thumbprint

    Invoke-Checked $signed @("--test")
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Invoke-Checked $signed @("--very-silent", "--no-run", "--output", $destination)

    $files = @(Get-ChildItem -LiteralPath $destination -File -Recurse)
    if ($files.Count -ne 1) {
        throw "Expected one extracted file, found $($files.Count)"
    }
    $content = [IO.File]::ReadAllText($files[0].FullName)
    if ($content -ne "Authenticode SFX regression payload`n") {
        throw "Extracted payload content did not match"
    }
    Write-Host "Authenticode SFX regression passed"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
