@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "OUT=%ROOT%\build\bin"
set "OBJ=%ROOT%\build\obj"
set "ENG=%OUT%\engines"

set "VCVARS="
for %%V in (
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
) do (
  if not defined VCVARS if exist %%V set "VCVARS=%%~V"
)

if not defined VCVARS (
  echo [x] vcvars64.bat not found. Install Visual Studio with C++ Desktop workload.
  exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
  echo [x] vcvars64 failed
  exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ENG%" mkdir "%ENG%"
if not exist "%OBJ%" mkdir "%OBJ%"

echo [1/3] icon + resources
if not exist "%ROOT%\src\host\app.ico" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\gen_icon.ps1" || exit /b 1
)
pushd "%ROOT%\src\host"
rc /nologo /fo "%OBJ%\app.res" app.rc || (popd & exit /b 1)
popd

set "CXXFLAGS=/nologo /std:c++17 /utf-8 /EHsc /W3 /O2 /MD /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%ROOT%\include""
set "LIBS=advapi32.lib user32.lib shell32.lib ole32.lib oleaut32.lib rpcrt4.lib rstrtmgr.lib"

echo [2/3] engines
call :engine basic     DeleteEngine.Basic
call :engine scan      DeleteEngine.Scan
call :engine freeze    DeleteEngine.Freeze
call :engine kill      DeleteEngine.Kill
call :engine service   DeleteEngine.Service
call :engine privilege DeleteEngine.Privilege
call :engine acl       DeleteEngine.Acl
call :engine nulldacl  DeleteEngine.Nulldacl
call :engine native    DeleteEngine.Native
call :engine deleteonclose DeleteEngine.Deleteonclose
call :engine globalroot DeleteEngine.Globalroot
call :engine staging   DeleteEngine.Staging
call :engine streams   DeleteEngine.Streams
call :engine reparse   DeleteEngine.Reparse
call :engine reboot    DeleteEngine.Reboot
call :engine squat     DeleteEngine.Squat
call :engine verify    DeleteEngine.Verify

echo [3/3] host exe
cl %CXXFLAGS% "%ROOT%\src\host\main.cpp" "%OBJ%\app.res" /Fo"%OBJ%\main.obj" /Fd"%OBJ%\host.pdb" /Fe"%OUT%\DeleteMaster.exe" /link %LIBS% > "%OBJ%\host.log" 2>&1
if errorlevel 1 (
  echo [x] host build failed, see %OBJ%\host.log
  type "%OBJ%\host.log"
  exit /b 1
)

echo.
echo Build OK: %OUT%\DeleteMaster.exe
dir /b "%ENG%"
exit /b 0

:engine
set "SRC=%ROOT%\src\engines\%~1\%~1.cpp"
cl %CXXFLAGS% /LD "%SRC%" /Fo"%OBJ%\%~1.obj" /Fd"%OBJ%\%~2.pdb" /Fe"%ENG%\%~2.dll" /link %LIBS% > "%OBJ%\%~2.log" 2>&1
if errorlevel 1 (
  echo [x] %~2 failed, see %OBJ%\%~2.log
  type "%OBJ%\%~2.log"
  exit /b 1
)
echo   ok  %~2.dll
exit /b 0
