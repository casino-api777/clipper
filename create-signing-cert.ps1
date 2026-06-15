# Creates a self-signed code-signing certificate for Hickory Phantom.
# Run once. Re-run only if you delete the cert or PFX.

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$pfxPath = Join-Path $PSScriptRoot "hickory-phantom-signing.pfx"
$cerPath = Join-Path $PSScriptRoot "hickory-phantom-signing.cer"
$pfxPassword = "HickoryDev"
$subject = "CN=Hickory Phantom, O=Hickory Phantom"

function Export-SigningCer {
    param($Certificate)
    Export-Certificate -Cert $Certificate -FilePath $cerPath -Type CERT | Out-Null
    Write-Host "  CER     : $cerPath"
}

if (Test-Path $pfxPath) {
    Write-Host "PFX already exists: $pfxPath"
    if (-not (Test-Path $cerPath)) {
        $secure = ConvertTo-SecureString -String $pfxPassword -Force -AsPlainText
        $cert = Get-PfxData -FilePath $pfxPath -Password $secure | Select-Object -ExpandProperty EndEntityCertificates | Select-Object -First 1
        Export-SigningCer $cert
    }
    exit 0
}

$cert = New-SelfSignedCertificate `
    -Type Custom `
    -Subject $subject `
    -FriendlyName "Hickory Phantom" `
    -KeyUsage DigitalSignature `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}") `
    -KeyExportPolicy Exportable `
    -KeySpec Signature `
    -KeyLength 2048 `
    -KeyAlgorithm RSA `
    -HashAlgorithm SHA256 `
    -NotAfter (Get-Date).AddYears(10) `
    -CertStoreLocation "Cert:\CurrentUser\My"

$secure = ConvertTo-SecureString -String $pfxPassword -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $secure | Out-Null
Export-SigningCer $cert

Write-Host "Created self-signed certificate:"
Write-Host "  Subject : $subject"
Write-Host "  Thumbprint: $($cert.Thumbprint)"
Write-Host "  PFX     : $pfxPath"
Write-Host "  Password: $pfxPassword"
Write-Host ""
Write-Host "Next: run sign-clip.ps1 to sign clip.exe"
Write-Host "For UAC publisher on this PC, run install-signing-trust.ps1 as Administrator."
