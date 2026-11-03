@echo off
rem Builds tests\crashreplay.exe (the end-to-end crash replay through carworld.cpp).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS crashreplay.cpp ..\carworld.cpp ..\carsim.cpp ..\physics.cpp ..\collision.cpp ..\city.cpp ..\contact.cpp ..\props.cpp ..\panels.cpp ..\articulated.cpp ..\vehicle.cpp ..\mdpc.cpp ..\damage.cpp ..\marks.cpp ..\smoke.cpp /Fe:crashreplay.exe
