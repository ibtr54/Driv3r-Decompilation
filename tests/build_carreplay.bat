@echo off
rem Builds tests\carreplay.exe (replays traced car dynamics calls through carsim.cpp).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS carreplay.cpp ..\carsim.cpp ..\physics.cpp ..\collision.cpp ..\city.cpp ..\camera.cpp ..\contact.cpp ..\articulated.cpp ..\vehicle.cpp ..\mdpc.cpp /Fe:carreplay.exe
