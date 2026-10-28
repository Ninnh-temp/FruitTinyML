@echo off
chcp 65001 >nul 2>&1
title Smart Scale POS - Hyprland Rice Edition Launcher
cd /d "%~dp0"

echo ==============================================================================
echo        SMART SCALE POS — HYPRLAND / CATPPUCCIN LINUX RICE EDITION
echo ==============================================================================
echo.
echo [1/2] Checking Python environment...
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python is not found in system PATH.
    echo Please install Python 3.8+ and ensure "Add Python to PATH" is checked.
    echo.
    pause
    exit /b 1
)

echo [2/2] Starting Local Python Disk Server (127.0.0.1:8765) ^& Native App Window...
echo - Direct CSV History Log : data\pos_orders_history.csv
echo - Persistent POS State   : data\pos_state.json
echo - Default ESP32-CAM IP   : 192.168.4.1
echo.

if exist "scripts\pos_desktop_app.py" (
    python scripts\pos_desktop_app.py %*
) else if exist "FruitTinyML\scripts\pos_desktop_app.py" (
    cd /d "%~dp0FruitTinyML"
    python scripts\pos_desktop_app.py %*
) else (
    echo [ERROR] Could not locate scripts\pos_desktop_app.py!
    pause
    exit /b 1
)

if errorlevel 1 (
    echo.
    echo [NOTICE] Application terminated with an error code.
    pause
)
