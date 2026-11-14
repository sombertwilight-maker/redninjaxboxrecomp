@echo off
rem Build redninja.exe with Visual Studio 2022 (C/C++ desktop workload) + CMake.
rem Output: bin\redninja.exe
setlocal
cd /d "%~dp0"
cmake -S . -B build || exit /b 1
cmake --build build --config Release --parallel || exit /b 1
echo.
echo Built bin\redninja.exe
