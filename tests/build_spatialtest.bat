@echo off
rem Builds tests\spatialtest.exe (the articulated solver's primitives against the game's code).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS spatialtest.cpp ..\articulated.cpp ..\physics.cpp /Fe:spatialtest.exe /link /BASE:0x10000000 /FIXED /DYNAMICBASE:NO
