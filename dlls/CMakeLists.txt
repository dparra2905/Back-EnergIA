# dlls/CMakeLists.txt - DLL de modelos de EnergIA, compiladas con MSVC.
#
# No se abre desde Qt Creator: se compila con compilar_dlls.bat, que prepara MSVC, descarga
# vcpkg (y con el, mlpack y sus dependencias) y copia las DLL a la carpeta bin/ del orquestador.
#
# Dependencias en modo ESTATICO (triplet x64-windows-static-md): mlpack, Armadillo, OpenBLAS
# y LAPACK quedan dentro de cada DLL. Asi no hay que copiar DLL extra a bin/, y LAPACK usa
# clapack (C puro) en vez de la version en Fortran que pediria el modo dinamico.
cmake_minimum_required(VERSION 3.21)
project(EnergIA_DLLs LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Las DLL de modelos se compilan con MSVC: usa dlls/compilar_dlls.bat")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# El contrato (energia_dll.h) y las ayudas comunes (energia_comun.h) estan en la raiz del proyecto
set(ENERGIA_RAIZ ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Mismas opciones de compilacion para TODAS las DLL de modelos, para que las diferencias de
# energia se deban al algoritmo y no al compilador. Se registran en compilacion_dlls.txt.
#   /O2 /Ob2  optimizacion de velocidad (lo normal en Release)
#   /EHsc     excepciones de C++ (ninguna sale de la DLL: se atrapan en cada funcion)
#   /bigobj   mlpack genera archivos objeto muy grandes
#   /utf-8    los fuentes estan en UTF-8
#   Sin /openmp y sin /arch:AVX2: un solo hilo y codigo que corre en cualquier x64
set(ENERGIA_OPCIONES_MSVC /O2 /Ob2 /EHsc /bigobj /utf-8 /permissive- /Zc:__cplusplus /W3)
set(ENERGIA_DEFINICIONES ENERGIA_DLL_EXPORTS NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES
                         ARMA_DONT_USE_OPENMP)

function(energia_dll objetivo nombre_salida fuente)
    add_library(${objetivo} SHARED ${fuente} ${ENERGIA_RAIZ}/energia_dll.h ${ENERGIA_RAIZ}/energia_comun.h)
    target_include_directories(${objetivo} PRIVATE ${ENERGIA_RAIZ})
    target_compile_definitions(${objetivo} PRIVATE ${ENERGIA_DEFINICIONES})
    target_compile_options(${objetivo} PRIVATE ${ENERGIA_OPCIONES_MSVC})
    set_target_properties(${objetivo} PROPERTIES OUTPUT_NAME ${nombre_salida} PREFIX "")
endfunction()

# ---- Arbol de decision (mlpack) ----
find_package(Armadillo CONFIG REQUIRED)
find_package(cereal CONFIG REQUIRED)
find_path(MLPACK_INCLUDE_DIRS "mlpack.hpp" REQUIRED)

energia_dll(energia_arbol EnergIA_Arbol energia_arbol.cpp)
target_include_directories(energia_arbol PRIVATE ${MLPACK_INCLUDE_DIRS})
target_link_libraries(energia_arbol PRIVATE armadillo cereal::cereal)

# ---- MLP (libtorch): entrega 3 ----

# Registro de como se compilo, para la tesis
file(WRITE ${CMAKE_BINARY_DIR}/bin/compilacion_dlls.txt
    "compilador: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
    "tipo: ${CMAKE_BUILD_TYPE}\n"
    "opciones: ${ENERGIA_OPCIONES_MSVC}\n"
    "definiciones: ${ENERGIA_DEFINICIONES}\n"
    "triplet vcpkg: ${VCPKG_TARGET_TRIPLET}\n")
