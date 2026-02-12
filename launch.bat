@echo off
chcp 65001 >nul
title MSHost by BaDRiVeR

set "CADDY_EXE=D:\Programms\MSHost\caddy\caddy.exe"
set "CADDY_CONFIG=D:\Programms\MSHost\caddy\Caddyfile"
set "CADDY_DIR=D:\Programms\MSHost\caddy"

rem --- Проверяем существование Caddy и конфига ---
if not exist "%CADDY_EXE%" (
    echo ERROR: Caddy not found at: %CADDY_EXE%
    pause
    exit /b 1
)

if not exist "%CADDY_CONFIG%" (
    echo ERROR: Caddyfile not found at: %CADDY_CONFIG%
    pause
    exit /b 1
)

rem --- Проверяем валидность конфига перед запуском ---
echo Проверяем конфигурацию Caddy...
"%CADDY_EXE%" validate --config "%CADDY_CONFIG%"
if errorlevel 1 (
    echo ERROR: Caddyfile содержит ошибки! Исправьте конфиг перед запуском.
    pause
    exit /b 1
)

rem --- Запускаем Caddy в фоне ---
echo Запускаем Caddy...
cd /d "%CADDY_DIR%"
start "" /b "%CADDY_EXE%" run --config "%CADDY_CONFIG%"

rem --- Ждем и ищем PID более надежным способом ---
echo Ожидаем запуск Caddy...
timeout /t 5 /nobreak >nul

set "CADDY_PID="
rem Ищем PID по конкретному пути исполняемого файла
for /f "tokens=2" %%I in (
    'wmic process where "name='caddy.exe' and ExecutablePath='%CADDY_EXE%'" get ProcessId /value ^| find "ProcessId"'
) do set "CADDY_PID=%%I"

if not defined CADDY_PID (
    echo WARNING: Не удалось найти PID Caddy через WMIC, пробуем альтернативный метод...
    for /f "tokens=2 delims=," %%a in ('tasklist /fi "imagename eq caddy.exe" /fo csv /nh') do (
        if not defined CADDY_PID set "CADDY_PID=%%~a"
    )
)

if not defined CADDY_PID (
    echo WARNING: Не удалось определить PID Caddy. Возможно, он не запустился.
) else (
    echo Caddy запущен. PID: %CADDY_PID%
)

rem --- Возвращаемся в исходную директорию ---
cd /d "%~dp0"

rem --- Проверяем существование mshost.exe ---
if not exist "bin\mshost.exe" (
    echo ERROR: mshost.exe not found in .bin/ directory!
    goto cleanup
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
set "APP_EXIT_CODE=!errorlevel!"
echo mshost.exe завершил работу с кодом: !APP_EXIT_CODE!

:cleanup
rem --- Завершаем Caddy ---
if defined CADDY_PID (
    echo Останавливаем Caddy (PID: %CADDY_PID%)...
    taskkill /PID %CADDY_PID% /F >nul 2>&1
    if errorlevel 1 (
        echo WARNING: Не удалось остановить Caddy. Возможно уже завершен.
    ) else (
        echo Caddy успешно остановлен.
    )
)

endlocal
echo.
if defined APP_EXIT_CODE (
    if !APP_EXIT_CODE! neq 0 (
        echo Программа завершена с ошибкой (код: !APP_EXIT_CODE!).
    ) else (
        echo Программа завершена успешно!
    )
) else (
    echo Сеанс завершен.
)

pause