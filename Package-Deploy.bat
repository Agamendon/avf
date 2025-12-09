@echo off
:: Post-build script to package AVF deployment files
:: Usage: Package-Deploy.bat <Configuration> <Platform> <SolutionDir>
:: Example: Package-Deploy.bat Debug x64 C:\av-filter\avf\

setlocal enabledelayedexpansion

set CONFIG=%1
set PLATFORM=%2
set SOLUTION_DIR=%~3

if "%CONFIG%"=="" set CONFIG=Debug
if "%PLATFORM%"=="" set PLATFORM=x64
if "%SOLUTION_DIR%"=="" set SOLUTION_DIR=%~dp0

echo.
echo ========================================
echo   Packaging AVF Deployment Files
echo   Configuration: %CONFIG%
echo   Platform: %PLATFORM%
echo ========================================
echo.

:: Set paths
set DEPLOY_DIR=%SOLUTION_DIR%deploy
set FILTER_OUT=%SOLUTION_DIR%filter\%PLATFORM%\%CONFIG%
set USER_OUT=%SOLUTION_DIR%user\%PLATFORM%\%CONFIG%

:: Alternative paths (Visual Studio sometimes uses different output structure)
if not exist "%FILTER_OUT%\avf.sys" (
    set FILTER_OUT=%SOLUTION_DIR%x64\%CONFIG%\avf
)
if not exist "%USER_OUT%\avf.exe" (
    set USER_OUT=%SOLUTION_DIR%x64\%CONFIG%\avf
)

:: Create deploy directory if not exists
if not exist "%DEPLOY_DIR%" mkdir "%DEPLOY_DIR%"

:: Copy driver
echo Copying avf.sys...
if exist "%FILTER_OUT%\avf.sys" (
    copy /Y "%FILTER_OUT%\avf.sys" "%DEPLOY_DIR%\" >nul
    echo   [OK] avf.sys
) else (
    echo   [SKIP] avf.sys not found at %FILTER_OUT%
)

:: Copy driver certificate
echo Copying avf.cer...
if exist "%FILTER_OUT%\avf.cer" (
    copy /Y "%FILTER_OUT%\avf.cer" "%DEPLOY_DIR%\" >nul
    echo   [OK] avf.cer
) else (
    echo   [SKIP] avf.cer not found at %FILTER_OUT%
)

:: Copy user-mode executable
echo Copying avf.exe...
if exist "%USER_OUT%\avf.exe" (
    copy /Y "%USER_OUT%\avf.exe" "%DEPLOY_DIR%\" >nul
    echo   [OK] avf.exe
) else (
    echo   [SKIP] avf.exe not found at %USER_OUT%
)

:: Copy INF file
echo Copying avf.inf...
if exist "%SOLUTION_DIR%avf.inf" (
    copy /Y "%SOLUTION_DIR%avf.inf" "%DEPLOY_DIR%\" >nul
    echo   [OK] avf.inf
) else (
    echo   [SKIP] avf.inf not found
)

:: Copy documentation
echo Copying documentation...
if exist "%SOLUTION_DIR%docs\SECURITY_CONSULTANT_PROTOCOL.md" (
    copy /Y "%SOLUTION_DIR%docs\SECURITY_CONSULTANT_PROTOCOL.md" "%DEPLOY_DIR%\" >nul
    echo   [OK] SECURITY_CONSULTANT_PROTOCOL.md
)

:: Ensure certificate scripts are present (they're already in deploy/)
echo Verifying certificate scripts...
if exist "%DEPLOY_DIR%\Setup-TestCert.ps1" (
    echo   [OK] Setup-TestCert.ps1
) else (
    echo   [SKIP] Setup-TestCert.ps1 not found
)
if exist "%DEPLOY_DIR%\Setup-TestCert.bat" (
    echo   [OK] Setup-TestCert.bat
) else (
    echo   [SKIP] Setup-TestCert.bat not found
)

echo.
echo Deployment package ready at: %DEPLOY_DIR%
echo.
echo Contents:
dir /B "%DEPLOY_DIR%"
echo.

:: Create ZIP archive
echo Creating ZIP archive...
set ZIP_FILE=%SOLUTION_DIR%avf-deploy.zip
if exist "%ZIP_FILE%" del "%ZIP_FILE%"
powershell -Command "Compress-Archive -Path '%DEPLOY_DIR%\*' -DestinationPath '%ZIP_FILE%' -Force"
if exist "%ZIP_FILE%" (
    echo   [OK] Created: %ZIP_FILE%
) else (
    echo   [SKIP] Failed to create ZIP
)
echo.
