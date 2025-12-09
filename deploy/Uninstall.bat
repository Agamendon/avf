@echo off
:: Uninstall AVF Driver - Run as Administrator

echo.
echo ========================================
echo   AVF Driver Uninstaller
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
powershell -ExecutionPolicy Bypass -File "%~dp0Uninstall-AVF.ps1"

echo.
pause
