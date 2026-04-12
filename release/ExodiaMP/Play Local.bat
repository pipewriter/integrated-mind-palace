@echo off
title Exodia MP - Local Server
cd /d "%~dp0"
echo Starting local server...
start "" /D "%~dp0" "%~dp0server.exe" --port 9998 --seed 42
timeout /t 2 /nobreak >nul
echo Connecting client...
start "" /D "%~dp0" "%~dp0client.exe" --host 127.0.0.1 --port 9998
