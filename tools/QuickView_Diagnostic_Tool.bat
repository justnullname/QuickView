@echo off
:: [CRITICAL FIX] Force the working directory to the script's actual location.
:: This prevents files from being saved to C:\Windows\System32 when running as Admin.
cd /d "%~dp0"

title QuickView Diagnostic Tool
setlocal

:: Check for Administrative Privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [!] ERROR: Please right-click this script and select "Run as Administrator".
    echo Reason: Windows requires special permission to start a diagnostic session.
    pause
    exit /b
)

:START_MENU
cls
echo ============================================================
echo           QuickView Diagnostic Log Collector
echo ============================================================
echo.
echo This tool helps record technical events to fix bugs.
echo Save Path: %~dp0
echo.
echo [ SAFETY ] No personal files or passwords are recorded.
echo.
echo ------------------------------------------------------------
echo 1. Start Recording (Reproduce the bug)
echo 2. Convert Log to Text (Review data before sending)
echo 3. Exit
echo ------------------------------------------------------------
set /p choice="Please choose an option (1-3): "

if "%choice%"=="3" exit /b
if "%choice%"=="1" goto RECORDING
if "%choice%"=="2" goto CONVERT

goto START_MENU

:RECORDING
cls
echo [ STEP 1/3 ] Initializing diagnostic session...
logman start QuickViewSession -p {a3a9c9e8-1d3a-4d5b-a15d-2498b73d6e5a} -o QuickView_Debug.etl -ets >nul 2>&1

if %errorLevel% neq 0 (
    echo [!] Failed to start. Attempting to reset...
    logman stop QuickViewSession -ets >nul 2>&1
    echo Please try running the script again.
    pause
    goto START_MENU
)

echo.
echo [ STEP 2/3 ] RECORDING IS ACTIVE!
echo ------------------------------------------------------------
echo ACTION REQUIRED:
echo 1. Keep this window open.
echo 2. Open QuickView and repeat the steps that caused the bug.
echo 3. Once the bug happens, come back here.
echo ------------------------------------------------------------
echo.
pause
echo.
echo [ STEP 3/3 ] Stopping and saving...
logman stop QuickViewSession -ets >nul 2>&1

echo.
echo SUCCESS! Diagnostic file 'QuickView_Debug.etl' created.
echo You can now send this file to the developer, or type '2' 
echo in the main menu to review its contents first.
pause
goto START_MENU

:CONVERT
cls
echo Converting QuickView_Debug.etl to readable text...
if not exist "QuickView_Debug.etl" (
    echo.
    echo [!] ERROR: Log file not found. 
    echo Please run Option 1 to record the bug first.
    echo.
    pause
    goto START_MENU
)

:: Use PowerShell to parse TraceLogging metadata correctly and export to a clean CSV
echo Parsing structured events (this may take a moment)...
powershell -Command "Get-WinEvent -Path 'QuickView_Debug.etl' -Oldest | Select-Object TimeCreated, Id, @{Name='Module';Expression={$_.Properties[0].Value}}, @{Name='Action';Expression={$_.Properties[1].Value}}, Message | Export-Csv -Path 'QuickView_Review.csv' -NoTypeInformation -Encoding utf8"

echo.
echo Conversion complete! 
echo File saved as: QuickView_Review.csv
echo.
echo Opening file for your review...
start "" "QuickView_Review.csv"
pause
goto START_MENU