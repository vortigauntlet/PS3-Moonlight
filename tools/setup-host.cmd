@echo off
rem Double-click launcher for setup-host.ps1 (Windows opens .ps1 files in Notepad).
rem Arguments pass through, e.g.  setup-host.cmd -DryRun
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-host.ps1" %*
echo.
pause
