@echo off
rem Builds wallp (Release) into build\. Needs Visual Studio 2019 or 2022 (or the Build Tools)
rem with the "Desktop development with C++" workload.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe was not found. Install Visual Studio or the Build Tools with the C++ workload.
    exit /b 1
)
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio installation with the x64 C++ tools was found.
    exit /b 1
)
rem (VS 2019's scripts print a harmless "vswhere.exe not found" on stderr, hence 2>nul.)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1

rem CMake and Ninja ship with the C++ workload; use those if none are on PATH.
where cmake >nul 2>nul || set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
where ninja >nul 2>nul || set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build" || exit /b 1
echo.
echo Built %~dp0build\wallp.exe (run it from there; shaders\ was copied next to it).
