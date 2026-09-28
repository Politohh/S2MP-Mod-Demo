@call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
@python -X utf8 tests\generate_cfg_dispatch_test.py
@if errorlevel 1 exit /b 1
@cl /nologo /EHsc /std:c++20 /O2 obj\test_cfg_dispatch.cpp /Fo:obj\test_cfg_dispatch.obj /Fe:obj\test_cfg_dispatch.exe
@if errorlevel 1 exit /b 1
@obj\test_cfg_dispatch.exe obj\cfg_dispatch-fixtures
