@echo off
rem Builds tests\mdpctest.exe (console check of mdpc.cpp).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS mdpctest.cpp ..\mdpc.cpp /Fe:mdpctest.exe
