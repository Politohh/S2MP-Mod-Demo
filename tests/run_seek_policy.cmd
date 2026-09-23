@call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
@cl /nologo /EHsc /std:c++20 tests\test_seek_policy.cpp /Fo:obj\test_seek_policy.obj /Fe:obj\test_seek_policy.exe
@if errorlevel 1 exit /b 1
@obj\test_seek_policy.exe
