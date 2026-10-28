@echo off
rem Builds tests\sprayreplay.exe (the car tick.s spray and tyre smoke against run 32).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS sprayreplay.cpp ..\carsim.cpp ..\physics.cpp ..\collision.cpp ..\city.cpp ..\camera.cpp ..\contact.cpp ..\props.cpp ..\panels.cpp ..\articulated.cpp ..\vehicle.cpp ..\mdpc.cpp ..\smoke.cpp /Fe:sprayreplay.exe
