# Signs clip.exe with the Hickory Phantom self-signed certificate.

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$exePath = Join-Path $PSScriptRoot "clip.exe"
$pfxPath = Join-Path $PSScriptRoot "hickory-phantom-signing.pfx"
$cerPath = Join-Path $PSScriptRoot "hickory-phantom-signing.cer"
$pfxPassword = "HickoryDev"

if (-not (Test-Path $exePath)) {
    Write-Error "clip.exe not found. Run build-clip.bat first."
}

if (-not (Test-Path $pfxPath)) {
    Write-Host "PFX missing. Creating certificate..."
    & (Join-Path $PSScriptRoot "create-signing-cert.ps1")
}

$secure = ConvertTo-SecureString -String $pfxPassword -Force -AsPlainText
$cert = Get-PfxData -FilePath $pfxPath -Password $secure | Select-Object -ExpandProperty EndEntityCertificates | Select-Object -First 1

if (-not $cert) {
    Write-Error "Failed to load certificate from $pfxPath"
}

if (-not (Test-Path $cerPath)) {
    Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT | Out-Null
}

$result = Set-AuthenticodeSignature -FilePath $exePath -Certificate $cert -HashAlgorithm SHA256
if ($result.Status -ne "Valid" -and $result.Status -ne "UnknownError") {
    Write-Warning "Signature status: $($result.Status)"
}

Write-Host "Signed: $exePath"
Write-Host "Publisher: Hickory Phantom"
Write-Host "Status: $($result.Status)"
Write-Host ""
Write-Host "UAC shows publisher Hickory Phantom after install (cert trust is installed on first elevated run)."
