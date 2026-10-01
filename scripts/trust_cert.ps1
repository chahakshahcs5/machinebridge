$sig = Get-AuthenticodeSignature "machinebridge-cpp\build\Release\machinebridge-server.exe"
$cert = $sig.SignerCertificate
if ($cert) {
    foreach ($name in @('TrustedPublisher', 'TrustedPeople', 'Root')) {
        try {
            $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($name, [System.Security.Cryptography.X509Certificates.StoreLocation]::CurrentUser)
            $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
            $store.Add($cert)
            $store.Close()
            Write-Host "Added cert to $name"
        } catch {
            Write-Warning ("Could not add to " + $name + ": " + $_)
        }
    }
}
& "machinebridge-cpp\scripts\sign_target.ps1" "machinebridge-cpp\build\Release\test_server.exe"
Get-AuthenticodeSignature "machinebridge-cpp\build\Release\test_server.exe" | Format-List Status, StatusMessage
