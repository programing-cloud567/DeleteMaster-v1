@echo off
setlocal
set "SRC=%~dp0"
set "DST=%LOCALAPPDATA%\Programs\DeleteMaster"

echo Installing DeleteMaster to "%DST%"
if exist "%DST%" rd /S /Q "%DST%"
mkdir "%DST%" 2>nul
copy /Y "%SRC%DeleteMaster.exe" "%DST%\" >nul
xcopy /E /I /Y "%SRC%engines" "%DST%\engines" >nul

if not exist "%DST%\DeleteMaster.exe" (
  echo ERROR: copy failed
  pause
  exit /b 1
)

echo Registering context menu...
reg add "HKCU\Software\Classes\*\shell\DeleteMaster" /ve /d "Delete (DeleteMaster)" /f >nul
reg add "HKCU\Software\Classes\*\shell\DeleteMaster" /v MUIVerb /d "Delete (DeleteMaster)" /f >nul
reg add "HKCU\Software\Classes\*\shell\DeleteMaster" /v Icon /d "%DST%\DeleteMaster.exe" /f >nul
reg add "HKCU\Software\Classes\*\shell\DeleteMaster" /v HasLUAShield /t REG_SZ /d "" /f >nul
reg add "HKCU\Software\Classes\*\shell\DeleteMaster" /v MultiSelectModel /d Single /f >nul
reg add "HKCU\Software\Classes\*\shell\DeleteMaster\command" /ve /d "\"%DST%\DeleteMaster.exe\" \"%%1\" /pause" /f >nul

reg add "HKCU\Software\Classes\Directory\shell\DeleteMaster" /ve /d "Delete (DeleteMaster)" /f >nul
reg add "HKCU\Software\Classes\Directory\shell\DeleteMaster" /v MUIVerb /d "Delete (DeleteMaster)" /f >nul
reg add "HKCU\Software\Classes\Directory\shell\DeleteMaster" /v Icon /d "%DST%\DeleteMaster.exe" /f >nul
reg add "HKCU\Software\Classes\Directory\shell\DeleteMaster" /v HasLUAShield /t REG_SZ /d "" /f >nul
reg add "HKCU\Software\Classes\Directory\shell\DeleteMaster" /v MultiSelectModel /d Single /f >nul
reg add "HKCU\Software\Classes\Directory\shell\DeleteMaster\command" /ve /d "\"%DST%\DeleteMaster.exe\" \"%%1\" /pause" /f >nul

echo.
echo Done. Right-click any file or folder - "Delete (DeleteMaster)".
echo On Windows 11 the item is under "Show more options".
pause
