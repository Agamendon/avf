@echo off
:: Install AVF Driver - Run as Administrator
:: This batch file launches the PowerShell installer with admin privileges

echo.
echo ========================================
echo   AVF Driver Installer
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
powershell -ExecutionPolicy Bypass -File "%~dp0Install-AVF.ps1"

echo.
pause
