@echo off
title Exodia MP - Arcade Server
cd /d "%~dp0"
echo Connecting to Exodia Arcade...
start "" /D "%~dp0" "%~dp0client.exe" --host 3.234.34.108 --port 10004
