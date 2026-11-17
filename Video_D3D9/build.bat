@echo off
REM Builds the 32-bit Video_D3D.dll GFXDLL plugin stub.
REM The game is a 32-bit process, so the x86 toolchain is mandatory.
REM d3d9.lib is deliberately NOT linked: Direct3DCreate9 is resolved at runtime so the
REM renderer can pick the d3d9.dll beside the game exe (RTX Remix) over the system one.

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

cl /nologo /LD /MT /O2 /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN ^
   "%~dp0src\config.cpp" "%~dp0src\dllmain.cpp" "%~dp0src\engine.cpp" "%~dp0src\device.cpp" "%~dp0src\exports.cpp" "%~dp0src\log.cpp" "%~dp0src\pcx.cpp" "%~dp0src\texture.cpp" "%~dp0src\geometry.cpp" "%~dp0src\normal_cache.cpp" "%~dp0src\lookdev.cpp" "%~dp0src\original.cpp" "%~dp0src\window.cpp" ^
   /Fe:Video_D3D.dll ^
   /link /MAP:Video_D3D.map /DEF:"%~dp0Video_D3D.def" /SUBSYSTEM:WINDOWS user32.lib gdi32.lib

set RC=%errorlevel%
popd

if %RC% neq 0 (
    echo BUILD FAILED
    exit /b %RC%
)

echo BUILD OK: %OUTDIR%\Video_D3D.dll
endlocal
