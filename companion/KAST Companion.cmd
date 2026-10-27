@echo off
rem KAST Companion — double-click to run the relay in the system tray.
rem The device finds this PC by itself (UDP broadcast beacon); no IP typing needed.
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0tray.ps1"
