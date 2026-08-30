[CmdletBinding()]
param(
    [switch]$Force,
    [string]$OpenSsl = "openssl"
)

$ErrorActionPreference = "Stop"

function Invoke-OpenSsl {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $OpenSsl @Arguments

    if ($LASTEXITCODE -ne 0) {
        throw "OpenSSL failed with exit code $LASTEXITCODE: $($Arguments -join ' ')"
    }
}

if (-not (Get-Command $OpenSsl -ErrorAction SilentlyContinue)) {
    throw "OpenSSL was not found in PATH. Open an ESP-IDF/Git environment that provides openssl, or pass -OpenSsl with the executable path."
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$CertDir = Join-Path $RepoRoot "components\web_server\certs"

New-Item -ItemType Directory -Path $CertDir -Force | Out-Null

$RootKey = Join-Path $CertDir "rootCA.key"
$RootCert = Join-Path $CertDir "rootCA.crt"
$RootSerial = Join-Path $CertDir "rootCA.srl"
$ServerKey = Join-Path $CertDir "prvtkey.pem"
$ServerCsr = Join-Path $CertDir "server.csr"
$ServerCert = Join-Path $CertDir "servercert.pem"
$ServerConfig = Join-Path $CertDir "server-openssl.cnf"

$GeneratedFiles = @(
    $RootKey,
    $RootCert,
    $RootSerial,
    $ServerKey,
    $ServerCsr,
    $ServerCert,
    $ServerConfig
)

$ExistingFiles = @(
    $GeneratedFiles | Where-Object { Test-Path $_ }
)

if ($ExistingFiles.Count -gt 0 -and -not $Force) {
    Write-Host "Existing GeoLocator TLS files were found:" -ForegroundColor Yellow
    $ExistingFiles | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
    Write-Host "Nothing was overwritten. Use -Force only if you intentionally want to replace the Root CA and server identity." -ForegroundColor Yellow
    exit 2
}

if ($Force) {
    $GeneratedFiles | ForEach-Object {
        if (Test-Path $_) {
            Remove-Item $_ -Force
        }
    }
}

$ServerConfigText = @"
[req]
distinguished_name = req_distinguished_name
req_extensions = v3_req
prompt = no

[req_distinguished_name]
C = UA
O = GeoLocator
CN = geolocator.local

[v3_req]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = geolocator.local
"@

Set-Content -Path $ServerConfig -Value $ServerConfigText -Encoding ascii

Write-Host "Generating GeoLocator Root CA..."
Invoke-OpenSsl -Arguments @(
    "genrsa",
    "-out", $RootKey,
    "4096"
)

Invoke-OpenSsl -Arguments @(
    "req",
    "-x509",
    "-new",
    "-nodes",
    "-key", $RootKey,
    "-sha256",
    "-days", "3650",
    "-out", $RootCert,
    "-subj", "/C=UA/O=GeoLocator/CN=GeoLocator Root CA"
)

Write-Host "Generating geolocator.local server key and CSR..."
Invoke-OpenSsl -Arguments @(
    "genrsa",
    "-out", $ServerKey,
    "2048"
)

Invoke-OpenSsl -Arguments @(
    "req",
    "-new",
    "-key", $ServerKey,
    "-out", $ServerCsr,
    "-config", $ServerConfig
)

Write-Host "Signing geolocator.local server certificate..."
Invoke-OpenSsl -Arguments @(
    "x509",
    "-req",
    "-in", $ServerCsr,
    "-CA", $RootCert,
    "-CAkey", $RootKey,
    "-CAcreateserial",
    "-out", $ServerCert,
    "-days", "825",
    "-sha256",
    "-extfile", $ServerConfig,
    "-extensions", "v3_req"
)

Write-Host "Verifying certificate chain..."
Invoke-OpenSsl -Arguments @(
    "verify",
    "-CAfile", $RootCert,
    $ServerCert
)

Write-Host ""
Write-Host "GeoLocator TLS credentials generated successfully." -ForegroundColor Green
Write-Host ""
Write-Host "ESP32 build inputs:"
Write-Host "  $ServerCert"
Write-Host "  $ServerKey"
Write-Host ""
Write-Host "Phone onboarding certificate (public):"
Write-Host "  $RootCert"
Write-Host ""
Write-Host "KEEP PRIVATE AND NEVER COMMIT/COPY TO ESP32:" -ForegroundColor Yellow
Write-Host "  $RootKey" -ForegroundColor Yellow
Write-Host ""
Write-Host "Next: run idf.py reconfigure, then idf.py build."
