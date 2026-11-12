@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1
set "BUILD_DIR=build"
if not "%~1"=="" set "BUILD_DIR=%~1"
rem The Visual Studio generator discovers Community or Build Tools itself.
cmake -S . -B "%BUILD_DIR%" -A x64
if errorlevel 1 goto :failed
cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 goto :failed
popd
exit /b 0

:failed
set "BUILD_ERROR=%ERRORLEVEL%"
popd
exit /b %BUILD_ERROR%
