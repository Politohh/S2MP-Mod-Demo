@call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
@python -X utf8 tests\generate_live_unload_test.py
@if errorlevel 1 exit /b 1
@cl /nologo /EHsc /std:c++20 /utf-8 /O2 obj\test_live_unload.cpp /Fo:obj\test_live_unload.obj /Fe:obj\test_live_unload.exe
@if errorlevel 1 exit /b 1
@obj\test_live_unload.exe
