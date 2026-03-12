@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_release_x86.ps1"
exit /b %errorlevel%
