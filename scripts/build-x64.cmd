@echo off
setlocal
REM ============================================================
REM  build-x64.cmd - Build Beautype x64 (64-bit TSF DLL + rime_server
REM  + register_tip + deployer). Usage: scripts\build-x64.cmd
REM
REM  Requires (see docs/dev-guide.md):
REM    - Visual Studio 2022 Build Tools (MSVC + Windows SDK)
REM    - librime source (env LIBRIME_SOURCE_DIR, default:
REM      third_party\librime submodule, fallback C:\dev\librime)
REM    - vcpkg triplet x64-windows-static with glog/gflags/leveldb
REM      (env VCPKG_INSTALLED_DIR, default C:\dev\vcpkg\installed)
REM    - weasel headers under third_party\weasel (run
REM      scripts\apply-weasel-patch.cmd first)
REM    - Boost headers at C:/boost_inc + b2 stage libs (see dev-guide)
REM
REM  Override env vars to point at your own toolchain.
REM ============================================================

cd /d %~dp0..

if "%VCVARS%"=="" set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if "%CMAKE_EXE%"=="" set CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe

call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo [ERR] vcvars64 failed - check VCVARS & exit /b 1 )

if "%LIBRIME_SOURCE_DIR%"=="" (
  if exist third_party\librime\CMakeLists.txt (
    set LIBRIME_SOURCE_DIR=%CD%\third_party\librime
  ) else (
    set LIBRIME_SOURCE_DIR=C:\dev\librime
  )
)
if "%VCPKG_INSTALLED_DIR%"=="" set VCPKG_INSTALLED_DIR=C:\dev\vcpkg\installed
set TRIPLET=x64-windows-static

echo [1/2] configure build (x64)...
"%CMAKE_EXE%" -S . -B build ^
  -DRIME_BEAUTIFY_BUILD_TESTS=ON ^
  -DLIBRIME_SOURCE_DIR=%LIBRIME_SOURCE_DIR% ^
  -DVCPKG_INSTALLED_DIR=%VCPKG_INSTALLED_DIR% ^
  -DVCPKG_TARGET_TRIPLET=%TRIPLET% ^
  -DCMAKE_PREFIX_PATH=%VCPKG_INSTALLED_DIR%/%TRIPLET% ^
  -DGlog_INCLUDE_PATH=%VCPKG_INSTALLED_DIR%/%TRIPLET%/include ^
  -DGflags_INCLUDE_PATH=%VCPKG_INSTALLED_DIR%/%TRIPLET%/include
if errorlevel 1 ( echo [ERR] configure failed & exit /b 1 )

echo [2/2] build targets rime_server rime_tsf register_tip deployer...
"%CMAKE_EXE%" --build build --config Release --target rime_server rime_tsf register_tip deployer --parallel
if errorlevel 1 ( echo [ERR] build failed & exit /b 1 )

echo [OK] x64 build done.
endlocal
