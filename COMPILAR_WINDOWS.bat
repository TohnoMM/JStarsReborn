@echo off
REM JStarsReborn - compilacion en Windows con un doble clic.
REM Descarga llvm-mingw 20261020 (UCRT x86_64, verificado en docs) y compila
REM el runtime con el fix del issue #7 (Ivy Bridge / Sandy Bridge).
REM Requiere: Windows 10/11 64-bit + Python 3.11 o superior en el PATH.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0COMPILAR_WINDOWS.ps1"
pause
