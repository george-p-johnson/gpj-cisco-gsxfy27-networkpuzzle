@echo off
setlocal

:: === GSXFY27 Network Puzzle - Show PC Startup Script ===
:: Launches the remote-controller Node server, then TouchDesigner with the
:: game project already loaded. Intended to run via Task Scheduler at logon.
::
:: The Cloudflare tunnel (gsxnetworkpuzzle.com -> localhost:8080) is NOT
:: started here -- it's installed as its own Windows service, which starts
:: it automatically at boot independent of this script.

set "PROJECT_ROOT=C:\_projects\gpj-cisco-gsxfy27-networkpuzzle"
set "REMOTE_DIR=%PROJECT_ROOT%\remote_controller"
set "NODE_EXE=C:\Program Files\nodejs\node.exe"
set "TOE_FILE=%PROJECT_ROOT%\GSXFY27_NetworkPuzzle.toe"
set "TD_EXE=C:\Program Files\Derivative\TouchDesigner.2025.32820\bin\TouchDesigner.exe"

:: Start the remote-controller Node server in its own window
start "GSX Remote Controller" /D "%REMOTE_DIR%" "%NODE_EXE%" server.js

:: Give the server a moment to bind its ports before TD comes up
timeout /t 3 /nobreak >nul

:: Start TouchDesigner with the show project loaded
start "TouchDesigner" "%TD_EXE%" "%TOE_FILE%"

endlocal
