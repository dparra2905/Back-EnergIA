@echo off
setlocal EnableExtensions
REM ==========================================================================================
REM compilar_dlls.bat - Compila las DLL de modelos de EnergIA con MSVC y las copia a bin/.
REM
REM Doble clic y listo. La PRIMERA vez descarga vcpkg y compila mlpack con sus dependencias
REM (OpenBLAS, LAPACK, Armadillo): puede tardar 15-40 minutos. Las siguientes, 1-2 minutos.
REM
REM Requisitos (se instalan una sola vez, a mano):
REM   - Build Tools de Visual Studio 2022 con "Desarrollo para el escritorio con C++"
REM   - Git para Windows
REM ==========================================================================================

REM ---- Configuracion ----
REM Version fija de vcpkg (y por lo tanto de mlpack 4.8.0 y sus dependencias): reproducible
set "VCPKG_COMMIT=8e89ad1130ff5c31ca1ff673842c645519a221ad"
REM Dependencias estaticas dentro de cada DLL, runtime de MSVC dinamico (/MD)
set "TRIPLET=x64-windows-static-md"
REM Carpeta bin del orquestador. Si se deja vacia, se copia a toda carpeta build\*\bin del
REM proyecto que tenga orquestador.exe (las que crea Qt Creator).
set "DIR_BIN_ORQUESTADOR="

set "DIR_DLLS=%~dp0"
set "DIR_DLLS=%DIR_DLLS:~0,-1%"
for %%I in ("%DIR_DLLS%\..") do set "DIR_PROYECTO=%%~fI"
set "DIR_VCPKG=%DIR_DLLS%\vcpkg"
set "DIR_BUILD=%DIR_DLLS%\build"

echo.
echo === EnergIA: compilar DLL de modelos ===
echo Proyecto: %DIR_PROYECTO%
echo.

REM ---- 1. Compilador MSVC ----
REM vswhere se ejecuta desde su carpeta: la ruta "Program Files (x86)" tiene parentesis y
REM dentro de un for /f hace que cmd pierda las comillas.
set "DIR_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%DIR_VSWHERE%\vswhere.exe" (
    echo ERROR: no se encontro Visual Studio ni sus Build Tools.
    echo Instala "Build Tools para Visual Studio" con "Desarrollo para el escritorio con C++".
    goto :error
)
REM Se elige la ultima instalacion que tenga el compilador de C++ ^(vcvars64.bat^)
REM -prerelease: incluye versiones preliminares ^(ej. Visual Studio 18 Insiders^)
set "VS_RUTA="
echo Instalaciones de Visual Studio encontradas:
pushd "%DIR_VSWHERE%"
for /f "usebackq tokens=*" %%i in (`vswhere.exe -all -prerelease -products * -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
        echo    %%i  [con compilador C++]
        set "VS_RUTA=%%i"
    ) else (
        echo    %%i  [SIN compilador C++]
    )
)
popd
if not defined VS_RUTA (
    echo ERROR: ninguna instalacion tiene el compilador de C++.
    echo Abre "Visual Studio Installer", pulsa "Modificar" en tus Build Tools y marca
    echo "Desarrollo para el escritorio con C++". Luego vuelve a correr este archivo.
    goto :error
)
echo [1/5] Preparando MSVC: %VS_RUTA%
call "%VS_RUTA%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :error

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: no se encontro CMake de Visual Studio.
    echo En el instalador de Visual Studio agrega el componente "Herramientas de CMake de C++ para Windows".
    goto :error
)
set "GENERADOR=Ninja"
where ninja >nul 2>&1
if errorlevel 1 set "GENERADOR=NMake Makefiles"

REM ---- 2. Git ----
where git >nul 2>&1
if errorlevel 1 (
    echo ERROR: no se encontro git. Instala "Git para Windows" y vuelve a correr este archivo.
    goto :error
)

REM ---- 3. vcpkg en la version fija ----
echo [2/5] Preparando vcpkg en %DIR_VCPKG%
if not exist "%DIR_VCPKG%\.git" (
    git clone https://github.com/microsoft/vcpkg.git "%DIR_VCPKG%"
    if errorlevel 1 goto :error
)
git -C "%DIR_VCPKG%" checkout -q %VCPKG_COMMIT% 2>nul
if errorlevel 1 (
    git -C "%DIR_VCPKG%" fetch -q origin
    git -C "%DIR_VCPKG%" checkout -q %VCPKG_COMMIT%
    if errorlevel 1 goto :error
)
if not exist "%DIR_VCPKG%\vcpkg.exe" (
    call "%DIR_VCPKG%\bootstrap-vcpkg.bat" -disableMetrics
    if errorlevel 1 goto :error
)

REM ---- 4. Configurar (aqui vcpkg instala mlpack la primera vez) y compilar ----
echo [3/5] Configurando con CMake (%GENERADOR%). La primera vez tarda: vcpkg compila las dependencias...
cmake -S "%DIR_DLLS%" -B "%DIR_BUILD%" -G "%GENERADOR%" -DCMAKE_BUILD_TYPE=Release ^
      "-DCMAKE_TOOLCHAIN_FILE=%DIR_VCPKG%\scripts\buildsystems\vcpkg.cmake" ^
      -DVCPKG_TARGET_TRIPLET=%TRIPLET%
if errorlevel 1 goto :error

echo [4/5] Compilando DLL de modelos...
cmake --build "%DIR_BUILD%" --config Release
if errorlevel 1 goto :error

REM ---- 5. Copiar a la carpeta del orquestador ----
echo [5/5] Copiando DLL a la carpeta del orquestador...
set /a COPIAS=0
if defined DIR_BIN_ORQUESTADOR (
    call :copiar "%DIR_BIN_ORQUESTADOR%"
) else (
    for /d %%D in ("%DIR_PROYECTO%\build\*") do (
        if exist "%%~D\bin\orquestador.exe" call :copiar "%%~D\bin"
    )
)
if %COPIAS%==0 (
    echo AVISO: no se encontro ninguna carpeta build\*\bin con orquestador.exe.
    echo Compila primero el orquestador en Qt Creator, o pon la ruta en DIR_BIN_ORQUESTADOR.
    echo Las DLL quedaron en %DIR_BUILD%\bin
)

echo.
echo === Listo. Abre el orquestador (o pulsa "Buscar modelos") ===
echo.
pause
exit /b 0

:copiar
copy /Y "%DIR_BUILD%\bin\EnergIA_*.dll" "%~1\" >nul
if errorlevel 1 (
    echo ERROR: no se pudo copiar a %~1 ^(esta abierto el orquestador?^)
    exit /b 1
)
copy /Y "%DIR_BUILD%\bin\compilacion_dlls.txt" "%~1\" >nul
echo    copiadas a %~1
set /a COPIAS+=1
exit /b 0

:error
echo.
echo *** La compilacion NO termino. Revisa el mensaje de arriba. ***
echo.
pause
exit /b 1