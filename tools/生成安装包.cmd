@echo off
rem Double-click this file to pack the game into a single exe.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pack_release.ps1" -Build %*
echo.
pause
