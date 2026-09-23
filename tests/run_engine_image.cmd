@python tests\generate_engine_image_test.py
@if errorlevel 1 exit /b 1
@call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
@cl /nologo /EHsc /std:c++20 /O2 tests\test_engine_image_policy.cpp /Fo:obj\test_engine_image_policy.obj /Fe:obj\test_engine_image_policy.exe
@if errorlevel 1 exit /b 1
@cl /nologo /EHsc /std:c++20 /O2 obj\test_engine_image_export.cpp /Fo:obj\test_engine_image_export.obj /Fe:obj\test_engine_image_export.exe
@if errorlevel 1 exit /b 1
@obj\test_engine_image_export.exe
