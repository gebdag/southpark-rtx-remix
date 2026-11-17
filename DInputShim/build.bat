@echo off
REM Builds the 32-bit dinput.dll input shim.
REM The game is a 32-bit process, so the x86 toolchain is mandatory.

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

cl /nologo /LD /MT /O2 /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /I"%~dp0..\common" ^
   "%~dp0src\shim.cpp" "%~dp0src\pad.cpp" "%~dp0src\cdaudio.cpp" "%~dp0src\rawmouse.cpp" ^
   /Fe:dinput.dll ^
   /link /MAP:dinput.map /DEF:"%~dp0dinput.def" user32.lib dinput8.lib dxguid.lib winmm.lib

set RC=%errorlevel%
popd

if %RC% neq 0 (
    echo BUILD FAILED
    exit /b %RC%
)

echo BUILD OK: %OUTDIR%\dinput.dll
endlocal
