@echo off
setlocal
cd /d C:\temp\VDJBufferTest
echo ============================================================
echo Building S4 MK3 NATIVE VDJ DISPLAY v38b - x64
echo V38b: native SQLite + plugin-local libusb
echo ============================================================
cl /nologo /EHsc /std:c++17 /LD /O2 /DNODLLEXPORT S4VDJDisplay_v38b.cpp /Fe:S4VDJDisplay_v38b.dll /link /OUT:S4VDJDisplay_v38b.dll
if errorlevel 1 (
  echo.
  echo BUILD FAILED
  exit /b 1
)
echo.
echo BUILD OK: S4VDJDisplay_v38b.dll
echo.
echo IMPORTANT:
echo Put libusb-1.0.dll in the SAME folder as S4VDJDisplay_v38b.dll
endlocal
