@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reset-ClawStage.ps1" %*
set "reset_result=%errorlevel%"
echo.
echo Log saved next to this launcher. This window closes in 10 seconds.
timeout /t 10 /nobreak >nul
exit /b %reset_result%
