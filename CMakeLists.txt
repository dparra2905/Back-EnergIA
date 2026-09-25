cmake_minimum_required(VERSION 3.16)
project(EnergIA LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# El orquestador, la DLL y el medidor de sistema quedan juntos en bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Definiciones de Windows para el orquestador y la DLL
# (system_meter.cpp define las suyas dentro del archivo)
set(ENERGIA_DEFS_WINDOWS _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

# ---- DLL de prueba (modelo simulado) ----
# Las DLL de modelos reales (arbol con mlpack, MLP con libtorch) se compilan aparte con MSVC
# desde la carpeta dlls/. Esta se queda aqui, con MinGW, para probar el orquestador sin
# dependencias. Cualquier DLL "EnergIA_*" en bin/ aparece sola en la lista de modelos.
add_library(energia_prueba SHARED energia_dll_stub.cpp energia_dll.h energia_comun.h)
target_compile_definitions(energia_prueba PRIVATE ENERGIA_DLL_EXPORTS)
set_target_properties(energia_prueba PROPERTIES
    OUTPUT_NAME "EnergIA_Prueba"
    PREFIX ""                      # MinGW agregaria "lib" al nombre
    CXX_VISIBILITY_PRESET hidden)
if(WIN32)
    target_compile_definitions(energia_prueba PRIVATE ${ENERGIA_DEFS_WINDOWS})
endif()
if(MINGW)
    # La DLL no depende de libstdc++-6.dll / libgcc_s en tiempo de ejecucion
    target_link_options(energia_prueba PRIVATE -static-libgcc -static-libstdc++)
endif()

# ---- Orquestador ----
# No se enlaza contra ninguna DLL de modelo: se buscan y cargan en ejecucion con QLibrary
add_executable(orquestador WIN32 orquestador.cpp energia_dll.h energia_comun.h)
target_link_libraries(orquestador PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
if(WIN32)
    target_compile_definitions(orquestador PRIVATE ${ENERGIA_DEFS_WINDOWS})
endif()
add_dependencies(orquestador energia_prueba)

# ---- Medidor de energia BLE (Python, carpeta medidores/) ----
# No se compila: el orquestador lo lanza con el Python del entorno virtual medidores/venv.
# La ruta de la carpeta se le pasa al orquestador al compilar.
set(ENERGIA_DIR_MEDIDORES "${CMAKE_SOURCE_DIR}/medidores")
target_compile_definitions(orquestador PRIVATE "ENERGIA_DIR_MEDIDORES=\"${ENERGIA_DIR_MEDIDORES}\"")

# Solo para que los archivos aparezcan y se editen desde Qt Creator
add_custom_target(medidor_energia SOURCES
    medidores/energia_ble.py
    medidores/requirements.txt
    medidores/crear_entorno.bat)

if(WIN32)
    set(ENERGIA_PYTHON_BLE "${ENERGIA_DIR_MEDIDORES}/venv/Scripts/python.exe")
else()
    set(ENERGIA_PYTHON_BLE "${ENERGIA_DIR_MEDIDORES}/venv/bin/python")
endif()
if(NOT EXISTS "${ENERGIA_PYTHON_BLE}")
    message(WARNING "No existe el entorno de Python del medidor de energia (${ENERGIA_PYTHON_BLE}). "
                    "Corre medidores/crear_entorno.bat una vez; mientras tanto las corridas se abortan "
                    "al lanzar el medidor 'energia'.")
endif()

# ---- Medidor de sistema (solo Windows) ----
# Proceso aparte, sin Qt. Queda en bin/ junto al orquestador, que lo lanza por nombre.
if(WIN32)
    add_executable(system_meter system_meter.cpp)
    target_link_libraries(system_meter PRIVATE pdh winmm shell32)
    if(MINGW)
        target_link_options(system_meter PRIVATE -static)  # no depende de DLLs de MinGW
    endif()
    add_dependencies(orquestador system_meter)
endif()