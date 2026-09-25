@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++20 /O2 tests\test_dolly_clock.cpp /Fo:obj\test_dolly_clock.obj /Fe:obj\test_dolly_clock.exe
if errorlevel 1 exit /b 1
obj\test_dolly_clock.exe
