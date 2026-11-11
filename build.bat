@echo off
rem Builds build\shaderscan.exe (Release) with the Visual Studio 2022 C++ build tools, CMake and Ninja.
rem Uses clang-cl when it is installed, else MSVC cl. Usage: build.bat [extra cmake -D options]
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (echo Visual Studio C++ build tools not found. & exit /b 1)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo Could not set up the Visual Studio build tools. & exit /b 1)
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
set "CC_OPT=-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl"
where clang-cl >nul 2>&1 && set "CC_OPT=-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl"
if not defined SHADERSCAN_JOBS set "SHADERSCAN_JOBS=2"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %CC_OPT% %*
if errorlevel 1 exit /b 1
cmake --build build -j %SHADERSCAN_JOBS%
if errorlevel 1 exit /b 1
build\shaderscan.exe --version
