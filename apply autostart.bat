@echo off
chcp 65001 >nul
cd /d "%~dp0"

:: Создаем задачу в Планировщике заданий от имени текущего пользователя с наивысшими правами
schtasks /create /tn "AquaWaveDynamicIsland" /tr "\"%~dp0DynamicIsland.exe\"" /sc onlogon /rl highest /f

if %errorlevel% equ 0 (
    echo [✔] Готово! DynamicIsland добавлен в автозапуск с правами администратора.
) else (
    echo [✖] Ошибка! Запусти этот файл от имени администратора (ПКМ - Запуск от имени администратора).
)
pause