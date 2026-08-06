@echo off
setlocal EnableExtensions
rem ============================================================================
rem  project-atlas - one-point setup (Windows dev machine).
rem
rem  Usage:  server\setup.bat
rem
rem  Idempotent: every step skips itself if it is already done, so re-running
rem  after a failure is safe and cheap.
rem
rem  Steps:
rem    1. locate the VS 2022 C++ toolset and put cmake / ninja / clang-* on PATH
rem    2. clone + bootstrap vcpkg into server\vcpkg
rem    3. pin server\vcpkg.json builtin-baseline to the cloned commit
rem    4. install the Node generator dependencies (npm)
rem    5. cmake --preset windows-debug  <-- this is the 20-60 minute step
rem
rem  Every step aborts with a one-line reason saying WHAT was missing.
rem  NOTE: only single-line constructs are used below; batch files in this repo
rem  are stored LF-only (.gitattributes) and multi-line blocks are the part of
rem  cmd.exe parsing that is fragile with LF line endings.
rem ============================================================================

set "SERVER_DIR=%~dp0"
set "REPO_ROOT=%SERVER_DIR%.."
set "VCPKG_DIR=%SERVER_DIR%vcpkg"

echo.
echo [setup] project-atlas one-point setup
echo [setup] server dir = %SERVER_DIR%
echo.

rem ---------------------------------------------------------------- 1. VS 2022
echo [setup] (1/5) %TIME% locating the Visual Studio 2022 C++ toolset ...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VSINSTALL (echo [setup] FAILED: no Visual Studio 2022 with the C++ toolset. Install VS 2022 with the "Desktop development with C++" workload. & exit /b 1)
echo [setup]       found: %VSINSTALL%

rem VsDevCmd.bat calls vswhere itself and prints a scary "not recognized" line if the VS Installer
rem directory is not on PATH. It still works, but the message reads like a failure - so put it there.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo
set "PATH=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VSINSTALL%\VC\Tools\Llvm\x64\bin;%PATH%"

where cl >nul 2>&1
if errorlevel 1 (echo [setup] FAILED: cl.exe not on PATH - the VS x64 dev environment did not initialise. Re-run from a "x64 Native Tools Command Prompt". & exit /b 1)
where cmake >nul 2>&1
if errorlevel 1 (echo [setup] FAILED: cmake.exe not found under %VSINSTALL%. Install the "C++ CMake tools for Windows" VS component. & exit /b 1)
where ninja >nul 2>&1
if errorlevel 1 (echo [setup] FAILED: ninja.exe not found under %VSINSTALL%. Install the "C++ CMake tools for Windows" VS component. & exit /b 1)
where clang-format >nul 2>&1
if errorlevel 1 (echo [setup] FAILED: clang-format.exe not found. Install the "C++ Clang tools for Windows" VS component - the format/tidy gate needs it. & exit /b 1)
where git >nul 2>&1
if errorlevel 1 (echo [setup] FAILED: git not on PATH - it is needed to clone vcpkg. & exit /b 1)
where npm >nul 2>&1
if errorlevel 1 (echo [setup] FAILED: npm not on PATH - install Node.js 22 or newer. & exit /b 1)
echo [setup]       toolchain OK: cl, cmake, ninja, clang-format, git, npm

rem ----------------------------------------------------------------- 2. vcpkg
echo.
echo [setup] (2/5) %TIME% vcpkg ...
if exist "%VCPKG_DIR%\.git" (echo [setup]       already cloned - skipping) else (git clone https://github.com/microsoft/vcpkg.git "%VCPKG_DIR%")
if errorlevel 1 (echo [setup] FAILED: could not clone https://github.com/microsoft/vcpkg.git - check network access / proxy. & exit /b 1)
if exist "%VCPKG_DIR%\vcpkg.exe" (echo [setup]       already bootstrapped - skipping) else (call "%VCPKG_DIR%\bootstrap-vcpkg.bat" -disableMetrics)
if errorlevel 1 (echo [setup] FAILED: bootstrap-vcpkg.bat did not produce vcpkg.exe. & exit /b 1)
if not exist "%VCPKG_DIR%\vcpkg.exe" (echo [setup] FAILED: %VCPKG_DIR%\vcpkg.exe is missing after bootstrap. & exit /b 1)

rem -------------------------------------------------------------- 3. baseline
echo.
echo [setup] (3/5) %TIME% pinning the vcpkg baseline into server\vcpkg.json ...
set "VCPKG_SHA="
for /f "usebackq delims=" %%i in (`git -C "%VCPKG_DIR%" rev-parse HEAD`) do set "VCPKG_SHA=%%i"
if not defined VCPKG_SHA (echo [setup] FAILED: could not read the vcpkg commit SHA - is %VCPKG_DIR% a git clone? & exit /b 1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%SERVER_DIR%scripts\set-vcpkg-baseline.ps1" -ManifestPath "%SERVER_DIR%vcpkg.json" -Sha %VCPKG_SHA%
if errorlevel 1 (echo [setup] FAILED: could not write builtin-baseline into server\vcpkg.json. & exit /b 1)

rem ------------------------------------------------------------------- 4. npm
echo.
echo [setup] (4/5) %TIME% installing the Node generator dependencies ...
pushd "%REPO_ROOT%"
if exist "package-lock.json" (call npm ci) else (call npm install)
if errorlevel 1 (echo [setup]       npm ci failed - falling back to npm install & call npm install)
if errorlevel 1 (popd & echo [setup] FAILED: npm could not install the tools/ dependencies. & exit /b 1)
popd

rem ----------------------------------------------------------------- 5. cmake
echo.
echo [setup] (5/5) %TIME% cmake --preset windows-debug
echo [setup]       This is the long one. vcpkg builds Boost, OpenSSL, MySQL, spdlog and
echo [setup]       gtest from source on the FIRST run only - budget 20-60 minutes.
echo [setup]       Later runs hit the local binary cache (%%LOCALAPPDATA%%\vcpkg\archives) and finish in seconds.
echo.
pushd "%SERVER_DIR%"
cmake --preset windows-debug
if errorlevel 1 (popd & echo [setup] FAILED: cmake configure failed. Read the vcpkg log path printed above - it names the port that did not build. & exit /b 1)
popd

echo.
echo [setup] %TIME% DONE.
echo [setup]   build   : cmake --build --preset windows-debug   (run from server\)
echo [setup]   CI gate : powershell -NoProfile -File server\scripts\ci-gate.ps1
echo.
exit /b 0
