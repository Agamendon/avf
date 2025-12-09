#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Installs the test signing certificate for the AVF driver.

.DESCRIPTION
    This script will:
    1. Enable test signing mode (if not already enabled)
    2. Install the avf.cer certificate to trusted stores
    3. Prompt for reboot if needed

.EXAMPLE
    .\Setup-TestCert.ps1
#>

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CertFile = Join-Path $ScriptDir "avf.cer"

function Write-Status {
    param([string]$Message, [string]$Color = "White")
    Write-Host "[*] $Message" -ForegroundColor $Color
}

function Write-Success {
    param([string]$Message)
    Write-Host "[+] $Message" -ForegroundColor Green
}

function Write-Error2 {
    param([string]$Message)
    Write-Host "[-] $Message" -ForegroundColor Red
}

# Check for admin
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error2 "This script requires Administrator privileges!"
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  AVF Test Certificate Setup" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

#
# Step 1: Check if certificate file exists
#
if (-not (Test-Path $CertFile)) {
    Write-Error2 "Certificate file not found: $CertFile"
    Write-Host "Make sure avf.cer is in the same folder as this script." -ForegroundColor Yellow
    exit 1
}

Write-Success "Found certificate: $CertFile"

#
# Step 2: Check if test signing is enabled
#
Write-Status "Checking test signing mode..."
$bcdedit = bcdedit /enum 2>$null | Select-String "testsigning\s+Yes"
if (-not $bcdedit) {
    Write-Status "Enabling test signing mode..." -Color Yellow
    bcdedit /set testsigning on | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Error2 "Failed to enable test signing. Try running:"
        Write-Host "    bcdedit /set testsigning on" -ForegroundColor Yellow
        exit 1
    }
    Write-Success "Test signing enabled. REBOOT REQUIRED!"
    $rebootRequired = $true
} else {
    Write-Success "Test signing is already enabled"
    $rebootRequired = $false
}

#
# Step 3: Install certificate to trusted stores
#
Write-Status "Installing certificate to trusted stores..."

# Check if already installed
$certData = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($CertFile)
$thumbprint = $certData.Thumbprint

$existingRoot = Get-ChildItem -Path Cert:\LocalMachine\Root | Where-Object { $_.Thumbprint -eq $thumbprint }
$existingPublisher = Get-ChildItem -Path Cert:\LocalMachine\TrustedPublisher | Where-Object { $_.Thumbprint -eq $thumbprint }

if ($existingRoot -and $existingPublisher) {
    Write-Success "Certificate already installed (Thumbprint: $thumbprint)"
} else {
    # Import to Root store
    if (-not $existingRoot) {
        Import-Certificate -FilePath $CertFile -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        Write-Success "Added to Trusted Root Certification Authorities"
    }
    
    # Import to TrustedPublisher store
    if (-not $existingPublisher) {
        Import-Certificate -FilePath $CertFile -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
        Write-Success "Added to Trusted Publishers"
    }
}

#
# Summary
#
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Setup Complete" -ForegroundColor Cyan  
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Certificate: $($certData.Subject)" -ForegroundColor White
Write-Host "Thumbprint:  $thumbprint" -ForegroundColor White
Write-Host ""

if ($rebootRequired) {
    Write-Host "*** REBOOT REQUIRED ***" -ForegroundColor Red
    Write-Host "Test signing was just enabled. Reboot before loading the driver." -ForegroundColor Yellow
    Write-Host ""
    $response = Read-Host "Reboot now? (y/N)"
    if ($response -eq 'y' -or $response -eq 'Y') {
        Restart-Computer -Force
    }
} else {
    Write-Host "Next steps:" -ForegroundColor Cyan
    Write-Host "  1. Run Install.bat to load the driver" -ForegroundColor White
    Write-Host "  2. Run avf.exe to start monitoring" -ForegroundColor White
    Write-Host ""
}
