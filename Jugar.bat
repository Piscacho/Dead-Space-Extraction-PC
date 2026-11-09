@echo off
rem Mejor usar el acceso directo "Dead Space Extraction" (no abre ninguna consola).
rem El juego escribe su registro en logs\juego.log.
start "" /d "%~dp0project\build" "%~dp0project\build\DSE.exe"
