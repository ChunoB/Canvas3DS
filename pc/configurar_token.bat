@echo off
chcp 65001 >nul
title Configurar o Canvas 3DS
where py >nul 2>nul
if errorlevel 1 (
    echo Python nao encontrado.
    echo Instale o Python 3 em https://www.python.org/downloads/
    echo e marque "Add Python to PATH" durante a instalacao.
    echo.
    pause
    exit /b 1
)
echo Coloque o cartao SD do 3DS no PC antes de continuar.
echo.
py -3 "%~dp0configurar_token.py"
echo.
pause
