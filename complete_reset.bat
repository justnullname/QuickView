@echo off
REM Complete Git Reset - Remove ALL history

echo ========================================
echo COMPLETE Git Repository Reset
echo ========================================
echo.
echo WARNING: This will COMPLETELY reset Git history!
echo.

SET /P CONFIRM="Continue? (y/N): "
if /I not "%CONFIRM%"=="y" (
    echo Aborted.
    exit /b 1
)

echo.
echo Step 1: Removing .git folder...
rd /s /q .git
rd /s /q .git.backup
echo Done.
echo.

echo Step 2: Initializing fresh repository...
git init
git branch -M main
echo Done.
echo.

echo Step 3: Adding all files...
git add .
echo Done.
echo.

echo Step 4: Creating clean commit...
git commit -m "Initial QuickView release - Modern image viewer for Windows"
echo Done.
echo.

echo Step 5: Adding remote...
git remote add origin https://github.com/justnullname/QuickView.git
echo Done.
echo.

echo ========================================
echo Ready to push!
echo ========================================
echo.
echo Run: git push -f origin main
echo.
