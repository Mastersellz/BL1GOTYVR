@echo off
call "C:\Program Files\Microsoft Visual Studio\2026\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
