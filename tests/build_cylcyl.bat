@echo off
rem Builds tests\cylcyl.exe (cylinder / cylinder contacts against the game's code).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS cylcyl.cpp ..\articulated.cpp ..\physics.cpp ..\panels.cpp ..\vehicle.cpp ..\mdpc.cpp ..\contact.cpp ..\props.cpp ..\collision.cpp ..\city.cpp /Fe:cylcyl.exe /link /BASE:0x10000000 /FIXED /DYNAMICBASE:NO
