@echo off
title TinyFruitML Dataset Collector
cd /d "%~dp0\.."
echo Launching TinyFruitML Dataset Collector GUI...
python scripts/collect_dataset_gui.py
if errorlevel 1 (
    echo.
    echo Script encountered an error. Press any key to exit.
    pause >nul
)
