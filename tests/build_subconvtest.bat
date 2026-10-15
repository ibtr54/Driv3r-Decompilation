@echo off
rem Builds tests\subconvtest.exe (prints the converted PCMP substances).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS subconvtest.cpp ..\mdpc.cpp /Fe:subconvtest.exe
