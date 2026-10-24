@echo off
setlocal
set "DST=%LOCALAPPDATA%\Programs\DeleteMaster"

echo Removing context menu...
reg delete "HKCU\Software\Classes\*\shell\DeleteMaster" /f >nul 2>&1
reg delete "HKCU\Software\Classes\Directory\shell\DeleteMaster" /f >nul 2>&1

echo Removing installed files...
taskkill /IM DeleteMaster.exe /F >nul 2>&1
if exist "%DST%" rd /S /Q "%DST%"

echo Done.
pause
