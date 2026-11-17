@echo off
REM Builds the 32-bit d3d9.dll Remix proxy. The game is a 32-bit process.

setlocal

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
    echo ERROR: Visual Studio not found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo ERROR: vcvars32 failed.
    exit /b 1
)

set OUTDIR=%~dp0build
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

pushd "%OUTDIR%"

cl /nologo /LD /MT /O2 /W3 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /I"%~dp0..\common" ^
   /I"%~dp0include" ^
   "%~dp0src\proxy.cpp" "%~dp0src\remix.cpp" "%~dp0src\lights.cpp" "%~dp0src\overlay.cpp" ^
   "%~dp0src\zones.cpp" "%~dp0src\settings.cpp" "%~dp0src\log.cpp" ^
   /Fe:d3d9.dll ^
   /link /MAP:d3d9.map /DEF:"%~dp0d3d9.def" user32.lib gdi32.lib

set RC=%errorlevel%
popd

if %RC% neq 0 (
    echo BUILD FAILED
    exit /b %RC%
)

echo BUILD OK: %OUTDIR%\d3d9.dll
endlocal
