@echo off
rem Builds Driv3rMenu.exe (32-bit: binkw32.dll is x86 only).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I"C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)\Include" main.cpp mec.cpp story.cpp level.cpp city.cpp mdpc.cpp mood.cpp treeshadow.cpp smoke.cpp vehicle.cpp render.cpp /Fe:Driv3rMenu.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib
