@call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
@python -X utf8 tests\generate_ncs_failure_test.py
@if errorlevel 1 exit /b 1
@cl /nologo /EHsc /std:c++20 /utf-8 /O2 obj\test_ncs_failure.cpp /Fo:obj\test_ncs_failure.obj /Fe:obj\test_ncs_failure.exe
@if errorlevel 1 exit /b 1
@obj\test_ncs_failure.exe
