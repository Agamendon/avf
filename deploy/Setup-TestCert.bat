@echo off
:: Setup Test Certificate for AVF Driver
:: This batch file creates and installs a test signing certificate

echo.
echo ========================================
echo   AVF Test Certificate Setup
echo ========================================
echo.

:: Check for admin privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo Requesting Administrator privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

:: Run the PowerShell script
powershell -ExecutionPolicy Bypass -File "%~dp0Setup-TestCert.ps1"

echo.
pause
