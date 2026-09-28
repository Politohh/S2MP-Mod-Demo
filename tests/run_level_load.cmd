@call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
@python -X utf8 tests\generate_level_load_test.py
@if errorlevel 1 exit /b 1
@cl /nologo /EHsc /std:c++20 /utf-8 /O2 obj\test_level_load.cpp /Fo:obj\test_level_load.obj /Fe:obj\test_level_load.exe
@if errorlevel 1 exit /b 1
@obj\test_level_load.exe
