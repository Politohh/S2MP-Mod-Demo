@call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
@python -X utf8 tests\generate_final_roll_test.py
@if errorlevel 1 exit /b 1
@cl /nologo /EHsc /std:c++20 /O2 obj\test_final_roll.cpp /Fo:obj\test_final_roll.obj /Fe:obj\test_final_roll.exe
@if errorlevel 1 exit /b 1
@obj\test_final_roll.exe
