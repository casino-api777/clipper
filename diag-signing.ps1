$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'clip.exe'
$cer = Join-Path $PSScriptRoot 'hickory-phantom-signing.cer'

Write-Host '=== clip.exe signature ==='
$sig = Get-AuthenticodeSignature $exe
$sig | Format-List Status, StatusMessage, SignatureType
if ($sig.SignerCertificate) {
    Write-Host "Signer: $($sig.SignerCertificate.Subject)"
    Write-Host "Thumbprint: $($sig.SignerCertificate.Thumbprint)"
}

Write-Host '=== embedded CER file ==='
if (Test-Path $cer) {
    $fileCer = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cer)
    Write-Host "CER Subject: $($fileCer.Subject)"
    Write-Host "CER Thumbprint: $($fileCer.Thumbprint)"
    if ($sig.SignerCertificate) {
        Write-Host "Thumbprints match: $($sig.SignerCertificate.Thumbprint -eq $fileCer.Thumbprint)"
    }
} else {
    Write-Host 'CER file missing'
}

Write-Host '=== trust stores ==='
$thumb = $sig.SignerCertificate.Thumbprint
$root = Get-ChildItem Cert:\LocalMachine\Root | Where-Object Thumbprint -eq $thumb
$pub = Get-ChildItem Cert:\LocalMachine\TrustedPublisher | Where-Object Thumbprint -eq $thumb
Write-Host "In LocalMachine\Root: $($null -ne $root)"
Write-Host "In LocalMachine\TrustedPublisher: $($null -ne $pub)"

$installed = Join-Path $env:LOCALAPPDATA 'Hickory Phantom\Clipper\clip.exe'
Write-Host '=== installed copy ==='
Write-Host "Path: $installed"
if (Test-Path $installed) {
    $isig = Get-AuthenticodeSignature $installed
    Write-Host "Status: $($isig.Status)"
    Write-Host "Signer thumbprint: $($isig.SignerCertificate.Thumbprint)"
    Write-Host "Matches build: $($isig.SignerCertificate.Thumbprint -eq $thumb)"
} else {
    Write-Host 'Not installed'
}
