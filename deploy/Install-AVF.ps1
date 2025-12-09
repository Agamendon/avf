#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Installs or updates the AVF (AV Filter) minifilter driver.

.DESCRIPTION
    This script will:
    1. Check if AVF is currently loaded
    2. Unload the existing driver if present
    3. Remove the old driver service
    4. Copy the new driver to System32\drivers
    5. Create the service and load the driver

.PARAMETER Uninstall
    If specified, only uninstalls the driver without reinstalling.

.EXAMPLE
    .\Install-AVF.ps1
    Installs or updates the AVF driver.

.EXAMPLE
    .\Install-AVF.ps1 -Uninstall
    Uninstalls the AVF driver.
#>

param(
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

$DriverName = "avf"
$ServiceName = "avf"
$DriverFileName = "avf.sys"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SourceDriver = Join-Path $ScriptDir $DriverFileName
$TargetDriver = Join-Path $env:SystemRoot "System32\drivers\$DriverFileName"

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

function Write-Warning2 {
    param([string]$Message)
    Write-Host "[!] $Message" -ForegroundColor Yellow
}

function Test-DriverLoaded {
    $filter = fltmc filters 2>$null | Select-String -Pattern "^\s*$DriverName\s+"
    return $null -ne $filter
}

function Test-ServiceExists {
    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    return $null -ne $service
}

function Unload-Driver {
    Write-Status "Unloading $DriverName filter..."
    $result = fltmc unload $DriverName 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Success "Driver unloaded successfully"
        return $true
    } else {
        Write-Warning2 "Driver was not loaded or failed to unload: $result"
        return $false
    }
}

function Remove-DriverService {
    Write-Status "Removing $ServiceName service..."
    
    # Stop service first
    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($service -and $service.Status -eq 'Running') {
        Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }
    
    # Delete service
    $result = sc.exe delete $ServiceName 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Success "Service removed successfully"
        return $true
    } else {
        Write-Warning2 "Service removal failed or not exists: $result"
        return $false
    }
}

function Install-Driver {
    # Check source file exists
    if (-not (Test-Path $SourceDriver)) {
        Write-Error2 "Driver file not found: $SourceDriver"
        return $false
    }
    
    # Copy driver to system32\drivers
    Write-Status "Copying driver to $TargetDriver..."
    try {
        Copy-Item -Path $SourceDriver -Destination $TargetDriver -Force
        Write-Success "Driver copied successfully"
    } catch {
        Write-Error2 "Failed to copy driver: $_"
        return $false
    }
    
    # Create service
    Write-Status "Creating $ServiceName service..."
    $result = sc.exe create $ServiceName `
        type= filesys `
        start= demand `
        binPath= $TargetDriver `
        group= "FSFilter Anti-Virus" `
        depend= FltMgr 2>&1
    
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1073) {  # 1073 = already exists
        Write-Error2 "Failed to create service: $result"
        return $false
    }
    Write-Success "Service created successfully"
    
    # Set up filter manager registration
    Write-Status "Configuring filter registration..."
    $regPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    $instancesPath = "$regPath\Instances"
    $instancePath = "$instancesPath\AVF Instance"
    
    # Create Instances key
    if (-not (Test-Path $instancesPath)) {
        New-Item -Path $instancesPath -Force | Out-Null
    }
    Set-ItemProperty -Path $instancesPath -Name "DefaultInstance" -Value "AVF Instance"
    
    # Create instance
    if (-not (Test-Path $instancePath)) {
        New-Item -Path $instancePath -Force | Out-Null
    }
    Set-ItemProperty -Path $instancePath -Name "Altitude" -Value "320000"
    Set-ItemProperty -Path $instancePath -Name "Flags" -Value 0 -Type DWord
    
    Write-Success "Filter registration configured"
    
    # Load driver
    Write-Status "Loading $DriverName filter..."
    $result = fltmc load $DriverName 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Success "Driver loaded successfully"
        return $true
    } else {
        Write-Error2 "Failed to load driver: $result"
        return $false
    }
}

function Show-Status {
    Write-Host ""
    Write-Status "Current filter status:" -Color Cyan
    fltmc filters | Select-String -Pattern "(Filter Name|$DriverName)" | ForEach-Object { Write-Host $_ }
    Write-Host ""
}

# Main execution
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  AVF Driver Installer" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check for admin
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error2 "This script requires Administrator privileges!"
    Write-Host "Right-click and select 'Run as Administrator'" -ForegroundColor Yellow
    exit 1
}

# Check test signing mode for development
$bcdedit = bcdedit /enum 2>$null | Select-String "testsigning\s+Yes"
if (-not $bcdedit) {
    Write-Warning2 "Test signing mode may not be enabled."
    Write-Host "    Run: bcdedit /set testsigning on" -ForegroundColor Yellow
    Write-Host "    Then reboot the system." -ForegroundColor Yellow
    Write-Host ""
}

if ($Uninstall) {
    Write-Status "Uninstalling AVF driver..." -Color Yellow
    
    if (Test-DriverLoaded) {
        Unload-Driver
    }
    
    if (Test-ServiceExists) {
        Remove-DriverService
    }
    
    if (Test-Path $TargetDriver) {
        Write-Status "Removing driver file..."
        Remove-Item -Path $TargetDriver -Force -ErrorAction SilentlyContinue
        Write-Success "Driver file removed"
    }
    
    Write-Host ""
    Write-Success "AVF driver uninstalled successfully!"
} else {
    Write-Status "Installing/Updating AVF driver..." -Color Yellow
    
    # Unload existing driver if loaded
    if (Test-DriverLoaded) {
        Write-Warning2 "Driver is currently loaded"
        Unload-Driver
        Start-Sleep -Milliseconds 500
    }
    
    # Remove existing service
    if (Test-ServiceExists) {
        Remove-DriverService
        Start-Sleep -Milliseconds 500
    }
    
    # Install new driver
    if (Install-Driver) {
        Write-Host ""
        Write-Success "AVF driver installed and loaded successfully!"
        Show-Status
        
        Write-Host "To start monitoring, run:" -ForegroundColor Cyan
        Write-Host "    .\avf.exe [file1] [file2] ..." -ForegroundColor White
        Write-Host ""
    } else {
        Write-Error2 "Installation failed!"
        exit 1
    }
}
