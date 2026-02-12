@echo off
chcp 65001 >nul
title MSHost by BaDRiVeR

cd /d "%~dp0"

rem --- Проверяем существование mshost.exe ---
if not exist "bin\mshost.exe" (
    echo ERROR: mshost.exe not found in bin\ directory!
    pause
    exit /b 1
)

rem --- Читаем аргументы из файла ---
setlocal enabledelayedexpansion
set "USER_ARGS=%*"
set "FILE_ARGS="

if exist "launch_args.txt" (
    echo Читаем аргументы из launch_args.txt...
    for /f "usebackq delims=" %%A in ("launch_args.txt") do (
        set "line=%%A"
        if not "!line!"=="" (
            set "FILE_ARGS=!FILE_ARGS! !line!"
        )
    )
)

rem --- Запускаем основное приложение ---
echo Запускаем mshost.exe...
bin\mshost.exe %USER_ARGS% %FILE_ARGS%
set "EXIT_CODE=!errorlevel!"

echo.
if !EXIT_CODE! neq 0 (
    echo Программа завершена с ошибкой, код: !EXIT_CODE!
) else (
    echo Программа завершена успешно!
)

endlocal
pause
