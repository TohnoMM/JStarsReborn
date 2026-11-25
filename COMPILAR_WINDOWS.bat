@echo off
REM JStarsReborn - compilacion en Windows con un doble clic.
REM Descarga llvm-mingw 20261020 (UCRT x64, verificado en docs) y compila
REM el runtime con el fix del issue #7 (Ivy Bridge / Sandy Bridge).
REM Requiere: Windows 10/11 64-bit + Python 3.11 o superior en el PATH.
REM
REM Videos: por defecto los videos del juego se SALTAN (sin decoder).
REM Para compilar con videos reales (FFmpeg), edita la linea de abajo y
REM anade -ConVideos al final (descarga ~85 MB la primera vez):
REM   powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0COMPILAR_WINDOWS.ps1" -ConVideos
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0COMPILAR_WINDOWS.ps1"
pause
