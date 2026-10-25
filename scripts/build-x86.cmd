@echo off
setlocal
REM ============================================================
REM  build-x86.cmd - Build Beautype x86 TSF (32-bit DLL for SysWOW64
REM  hosts like WPS) + register_tip. Usage: scripts\build-x86.cmd
REM
REM  IMPORTANT: x86 tree only builds rime_tsf + register_tip
REM  (RIME_BEAUTIFY_BUILD_TSF_X86=ON skips tests; engine lives in the
REM  64-bit rime_server). Do NOT run a full build-x86 build - deployer
REM  unconditionally links rime_engine and fails with LNK1181 on the x86
REM  tree (see docs/errors-and-lessons.md 25.2).
REM
REM  Requires (see docs/dev-guide.md):
REM    - vcpkg triplet x86-windows-static
REM    - Boost b2 stage-x86 libs (address-model=32), passed via
REM      -DBOOST_STAGE_LIB=...\stage-x86\lib
REM
REM  Override env vars to point at your own toolchain.
REM ============================================================

cd /d %~dp0..

if "%VCVARS%"=="" set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat
if "%CMAKE_EXE%"=="" set CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe

call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo [ERR] vcvars32 failed - check VCVARS & exit /b 1 )

if "%VCPKG_INSTALLED_DIR%"=="" set VCPKG_INSTALLED_DIR=C:\dev\vcpkg\installed
if "%BOOST_STAGE_LIB%"=="" set BOOST_STAGE_LIB=C:\boost_src\boost_1_87_0\stage-x86\lib
set TRIPLET=x86-windows-static

echo [1/2] configure build-x86 (Win32, TSF only)...
"%CMAKE_EXE%" -S . -B build-x86 -A Win32 ^
  -DRIME_BEAUTIFY_BUILD_TSF_X86=ON ^
  -DVCPKG_INSTALLED_DIR=%VCPKG_INSTALLED_DIR% ^
  -DVCPKG_TARGET_TRIPLET=%TRIPLET% ^
  -DCMAKE_PREFIX_PATH=%VCPKG_INSTALLED_DIR%/%TRIPLET% ^
  -DGlog_INCLUDE_PATH=%VCPKG_INSTALLED_DIR%/%TRIPLET%/include ^
  -DGflags_INCLUDE_PATH=%VCPKG_INSTALLED_DIR%/%TRIPLET%/include ^
  -DBOOST_STAGE_LIB=%BOOST_STAGE_LIB%
if errorlevel 1 ( echo [ERR] configure failed & exit /b 1 )

echo [2/2] build targets rime_tsf register_tip...
"%CMAKE_EXE%" --build build-x86 --config Release --target rime_tsf register_tip --parallel
if errorlevel 1 ( echo [ERR] build failed & exit /b 1 )

echo [OK] x86 build done. Copy build-x86\src\ime\Release\rime_tsf.dll to packaging\Beautype\x86\ before packaging.
endlocal
