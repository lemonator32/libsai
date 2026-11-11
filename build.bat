@echo off
rem Build libsai with MSVC without CMake.  Run from a normal command prompt;
rem the Visual Studio environment is located with vswhere.
setlocal
if defined VCINSTALLDIR goto :have_env
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_env
if not exist build mkdir build
set CFLAGS=/nologo /O2 /W4 /Iinclude /D_CRT_SECURE_NO_WARNINGS
pushd build
cl %CFLAGS% /I..\include /c ..\src\crypto.c ..\src\document.c ..\src\key.c ..\src\rle.c ..\src\util.c ..\src\vfs_read.c ..\src\vfs_write.c || goto :fail
lib /nologo /out:sai.lib crypto.obj document.obj key.obj rle.obj util.obj vfs_read.obj vfs_write.obj || goto :fail
cl %CFLAGS% /I..\include ..\tools\saitool.c sai.lib /Fe:saitool.exe || goto :fail
cl %CFLAGS% /I..\include ..\tests\test_main.c sai.lib /Fe:test_libsai.exe || goto :fail
popd
echo Build OK: build\sai.lib build\saitool.exe build\test_libsai.exe
exit /b 0
:fail
popd
exit /b 1
