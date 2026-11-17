@echo off
rem Builds build\LA_FastImport.dll (32-bit, static CRT) and build\lafi_harness.exe (test harness).
rem Needs Visual Studio 2019 or newer with the C++ x86 tools; the newest installed one is used.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Install Visual Studio with the C++ workload.
    exit /b 1
)
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio with C++ tools found.
    exit /b 1
)
echo Using %VSDIR%
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
if not exist build mkdir build
cl /nologo /O2 /MT /W4 /EHa /std:c++17 /DWIN32 /D_CRT_SECURE_NO_WARNINGS /LD src\la_fastimport.cpp /Fo:build\ /Fe:build\LA_FastImport.dll /link /SAFESEH:NO || exit /b 1
if /i "%~1"=="dll" exit /b 0
cl /nologo /O2 /MT /W3 /EHa /std:c++17 /DWIN32 /D_CRT_SECURE_NO_WARNINGS test\harness.cpp /Fo:build\ /Fe:build\lafi_harness.exe /link /SAFESEH:NO /LARGEADDRESSAWARE /FIXED /BASE:0x20000000 || exit /b 1
exit /b 0
