@echo off
setlocal

:: === GSXFY27 Network Puzzle - Show PC Startup Script ===
:: Launches the remote-controller Node server and TouchDesigner, each
:: wrapped in its own watchdog window that auto-restarts it if it ever
:: exits (crash recovery -- see run_server_watchdog.bat / run_td_watchdog.bat).
:: Intended to run via Task Scheduler at logon.
::
:: The Cloudflare tunnel (gsxnetworkpuzzle.com -> localhost:8080) is NOT
:: started here -- it's installed as its own Windows service, which starts
:: it automatically at boot independent of this script.

set "PROJECT_ROOT=C:\_projects\gpj-cisco-gsxfy27-networkpuzzle"

:: Start the remote-controller server watchdog
start "GSX Remote Controller (watchdog)" "%PROJECT_ROOT%\run_server_watchdog.bat"

:: Give the server a moment to bind its ports before TD comes up
timeout /t 3 /nobreak >nul

:: Start the TouchDesigner watchdog
start "TouchDesigner (watchdog)" "%PROJECT_ROOT%\run_td_watchdog.bat"

endlocal
