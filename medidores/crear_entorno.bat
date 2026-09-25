@echo off
REM Crea el entorno virtual del medidor BLE de EnergIA junto a este archivo.
setlocal
cd /d "%~dp0"

if exist venv\Scripts\python.exe (
    echo El entorno ya existe en %CD%\venv. Se actualizan las dependencias.
) else (
    echo Creando entorno virtual en %CD%\venv ...
    python -m venv venv
    if errorlevel 1 (
        echo No se pudo crear el entorno. Revisa que Python 3.13 este instalado y en el PATH.
        exit /b 1
    )
)

venv\Scripts\python.exe -m pip install --upgrade pip
venv\Scripts\python.exe -m pip install -r requirements.txt
if errorlevel 1 (
    echo Fallo la instalacion de dependencias.
    exit /b 1
)

echo.
venv\Scripts\python.exe --version
venv\Scripts\python.exe -c "import importlib.metadata as m; print('bleak', m.version('bleak'))"
echo Entorno listo.
endlocal
