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

function Find-CertUtil {
    $fromPath = Get-Command certutil.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty Source
    if (-not $fromPath) { throw "Windows certutil.exe was not found" }
    return $fromPath
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

$signTool = Find-SignTool
$certUtil = Find-CertUtil
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
    $cer = Join-Path $temp "test-signing.cer"
    $destination = Join-Path $temp "extracted"

    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($inputFile, "Authenticode SFX regression payload`n", $utf8)
    Invoke-Checked $axiomc @("a", $archive, $inputFile)
    Invoke-Checked $axiomc @("sfx", "--stub", "mini", $archive, $unsigned)

    $certificateParameters = @{
        Type = "CodeSigningCert"
        Subject = "CN=AxiomCompress CI SFX"
        CertStoreLocation = "Cert:\CurrentUser\My"
        HashAlgorithm = "SHA256"
        KeyExportPolicy = "Exportable"
        NotAfter = (Get-Date).AddDays(1)
    }
    $certificate = New-SelfSignedCertificate @certificateParameters
    $thumbprint = $certificate.Thumbprint
    $passwordText = [guid]::NewGuid().ToString("N")
    $password = ConvertTo-SecureString $passwordText -AsPlainText -Force
    Export-PfxCertificate -Cert $certificate -FilePath $pfx -Password $password | Out-Null
    Export-Certificate -Cert $certificate -FilePath $cer | Out-Null
    # Import-Certificate can display a trust prompt on some developer images.
    # certutil's user-store path is deterministic and does not require UI input.
    Invoke-Checked $certUtil @("-user", "-addstore", "-f", "Root", $cer)
    Invoke-Checked $certUtil @("-user", "-addstore", "-f", "TrustedPublisher", $cer)

    Copy-Item -LiteralPath $unsigned -Destination $signed
    Invoke-Checked $signTool @(
        "sign", "/fd", "SHA256", "/f", $pfx, "/p", $passwordText,
        "/d", "AxiomCompress CI SFX", $signed
    )
    Invoke-Checked $signTool @("verify", "/pa", "/all", $signed)

    $signature = Get-AuthenticodeSignature -LiteralPath $signed
    if ("$($signature.Status)" -ne "Valid") {
        throw "Authenticode signature status was $($signature.Status): $($signature.StatusMessage)"
    }

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
    if ($thumbprint) {
        foreach ($store in @(
            "My",
            "Root",
            "TrustedPublisher"
        )) {
            & $certUtil "-user" "-delstore" $store $thumbprint *> $null
        }
    }
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
