@echo off
title GSX Remote Controller (auto-restart)
setlocal

set "REMOTE_DIR=C:\_projects\gpj-cisco-gsxfy27-networkpuzzle\remote_controller"
set "NODE_EXE=C:\Program Files\nodejs\node.exe"

cd /d "%REMOTE_DIR%"

echo This window auto-restarts the remote-controller server if it ever exits.
echo Close THIS window to stop that -- closing/killing just the server won't.
echo.

set /a COUNT=0

:loop
set /a COUNT+=1
echo [%DATE% %TIME%] Starting server (attempt %COUNT%)...
"%NODE_EXE%" server.js
echo [%DATE% %TIME%] Server exited. Restarting in 5 seconds...
echo.
timeout /t 5 /nobreak >nul
goto loop
