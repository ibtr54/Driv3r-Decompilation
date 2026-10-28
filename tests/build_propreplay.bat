@echo off
rem Builds tests\propreplay.exe (the smashable props against run 25).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS propreplay.cpp ..\articulated.cpp ..\physics.cpp ..\panels.cpp ..\vehicle.cpp ..\mdpc.cpp ..\contact.cpp ..\props.cpp ..\collision.cpp ..\city.cpp /Fe:propreplay.exe
