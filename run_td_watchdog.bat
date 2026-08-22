@echo off
title TouchDesigner (auto-restart)
setlocal

set "TD_EXE=C:\Program Files\Derivative\TouchDesigner.2025.32820\bin\TouchDesigner.exe"
set "TOE_FILE=C:\_projects\gpj-cisco-gsxfy27-networkpuzzle\GSXFY27_NetworkPuzzle.toe"

echo This window auto-restarts TouchDesigner if it ever exits (crash OR
echo a deliberate close -- e.g. at end-of-night teardown, close THIS
echo window FIRST, otherwise TD will just pop back open).
echo.

set /a COUNT=0

:loop
set /a COUNT+=1
echo [%DATE% %TIME%] Starting TouchDesigner (attempt %COUNT%)...
start "TouchDesigner" /wait "%TD_EXE%" "%TOE_FILE%"
echo [%DATE% %TIME%] TouchDesigner exited. Restarting in 5 seconds...
echo.
timeout /t 5 /nobreak >nul
goto loop
