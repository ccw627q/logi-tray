@echo off
setlocal

rem logi-tray build script (MinGW-w64)
set "MINGW=E:\mingw64"
set "SRC=src"
set "OUT=bin"

if not exist "%OUT%" mkdir "%OUT%"

rem Step 1: compile resources separately (windres cannot handle the .rc directly in g++)
"%MINGW%\bin\windres.exe" res\app.rc -O coff -o "%OUT%\app_res.o"
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Resource compilation FAILED
    exit /b 1
)

rem Step 2: compile and link (static runtime, GUI subsystem so no console window appears)
"%MINGW%\bin\g++.exe" -O2 -std=c++17 -municode -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ -mwindows ^
  -I"%SRC%" ^
  "%SRC%\main.cpp" ^
  "%SRC%\logi_protocol.cpp" ^
  "%SRC%\device_manager.cpp" ^
  "%SRC%\tray_menu.cpp" ^
  "%SRC%\osd_window.cpp" ^
  "%SRC%\alert_window.cpp" ^
  "%SRC%\mock_hidpp.cpp" ^
  "%OUT%\app_res.o" ^
  -o "%OUT%\logi-tray.exe" ^
  -lsetupapi -lhid -ldwmapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32

if %ERRORLEVEL% == 0 (
    echo.
    echo Build OK: %OUT%\logi-tray.exe
) else (
    echo.
    echo Build FAILED
)

endlocal
