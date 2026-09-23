@call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
@python -X utf8 tests\generate_hud_state_test.py
@if errorlevel 1 exit /b 1
@cl /nologo /EHsc /std:c++20 /O2 /I src obj\test_hud_state.cpp /Fo:obj\test_hud_state.obj /Fe:obj\test_hud_state.exe
@if errorlevel 1 exit /b 1
@obj\test_hud_state.exe
@if errorlevel 1 exit /b 1
@cl /nologo /EHsc /std:c++20 /O2 /I src tests\test_engine_probe.cpp /Fo:obj\test_engine_probe.obj /Fe:obj\test_engine_probe.exe
@if errorlevel 1 exit /b 1
@obj\test_engine_probe.exe
