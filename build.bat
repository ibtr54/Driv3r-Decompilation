@echo off
rem Builds Driv3rMenu.exe (32-bit: binkw32.dll is x86 only).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS main.cpp mec.cpp story.cpp /Fe:Driv3rMenu.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib
