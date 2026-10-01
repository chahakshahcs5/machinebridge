param([string]$FilePath)
if (-not $FilePath -or -not (Test-Path $FilePath)) { return }
$cert = Get-ChildItem 'Cert:\CurrentUser\My' -CodeSigningCert | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=DevMachineBridge' -CertStoreLocation 'Cert:\CurrentUser\My'
}
Set-AuthenticodeSignature -FilePath $FilePath -Certificate $cert | Out-Null
