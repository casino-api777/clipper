# Installs the Hickory Phantom self-signed cert into trusted stores on THIS machine.
# Run PowerShell as Administrator once per PC.

#Requires -RunAsAdministrator

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$pfxPath = Join-Path $PSScriptRoot "hickory-phantom-signing.pfx"
$pfxPassword = "HickoryDev"

if (-not (Test-Path $pfxPath)) {
    Write-Error "PFX not found. Run create-signing-cert.ps1 first."
}

$secure = ConvertTo-SecureString -String $pfxPassword -Force -AsPlainText
$cert = Get-PfxData -FilePath $pfxPath -Password $secure | Select-Object -ExpandProperty EndEntityCertificates | Select-Object -First 1

$rootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", "LocalMachine")
$rootStore.Open("ReadWrite")
try {
    if (-not $rootStore.Certificates.Contains($cert)) {
        $rootStore.Add($cert)
        Write-Host "Added to Trusted Root Certification Authorities."
    } else {
        Write-Host "Already in Trusted Root Certification Authorities."
    }
} finally {
    $rootStore.Close()
}

$pubStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPublisher", "LocalMachine")
$pubStore.Open("ReadWrite")
try {
    if (-not $pubStore.Certificates.Contains($cert)) {
        $pubStore.Add($cert)
        Write-Host "Added to Trusted Publishers."
    } else {
        Write-Host "Already in Trusted Publishers."
    }
} finally {
    $pubStore.Close()
}

Write-Host ""
Write-Host "Done. UAC should now show publisher: Hickory Phantom (on this PC only)."
