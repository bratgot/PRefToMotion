@echo off
setlocal

rem rez provides these in the build environment.
set "SRC=%REZ_BUILD_SOURCE_PATH%"
set "INSTALL=%REZ_BUILD_INSTALL_PATH%"

rem Configure with the Visual Studio generator. MSBuild locates cl.exe on its own,
rem so no vcvars / nmake are required in the rez context. Change "Visual Studio 17
rem 2022" if you build with a different VS. To match Foundry's toolset exactly you
rem can append:  -T v142   (needs the VS2019 build tools installed).
cmake "%SRC%" -G "Visual Studio 17 2022" -A x64 -DCMAKE_INSTALL_PREFIX="%INSTALL%"
if errorlevel 1 exit /b 1

cmake --build . --config Release
if errorlevel 1 exit /b 1

if /I "%~1"=="install" (
    cmake --install . --config Release
    if errorlevel 1 exit /b 1
)
