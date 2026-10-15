@echo off
rem Builds tests\fxdump.exe (lists the effects inside Driv3r.exe; needs the
rem DirectX SDK June 2010 headers and d3dx9_43.dll next to the program).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS /I"C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)\Include" fxdump.cpp /Fe:fxdump.exe /link d3d9.lib user32.lib
