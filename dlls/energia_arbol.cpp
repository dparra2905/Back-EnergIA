// energia_arbol.cpp - EnergIA_Arbol.dll: arbol de decision (CART) con mlpack, contrato v3.
//
// El arbol lo construye mlpack (DecisionTree): esta DLL solo traduce el contrato de EnergIA
// a la API de mlpack. Se compila con MSVC desde dlls/ (ver compilar_dlls.bat).
//
// - Un solo paso: el arbol se construye de una sola vez. mlpack no tiene forma de avisar
//   durante la construccion, asi que una cancelacion pedida a mitad se atiende al terminar.
// - Metricas del paso (salen gratis del arbol ya construido): profundidad y hojas.
// - evaluar(): val_accuracy (se llama con los medidores ya detenidos).
// - Un solo hilo: se compila sin OpenMP y con BLAS de un solo hilo.

#ifndef ENERGIA_DLL_EXPORTS
#define ENERGIA_DLL_EXPORTS
#endif
#include "energia_dll.h"
#include "energia_comun.h"

#include <mlpack/core.hpp>
#include <mlpack/methods/decision_tree.hpp>

#include <cereal/archives/binary.hpp>

#include <cstdio>
#include <memory>
#include <string>

namespace {

using ArbolGini = mlpack::DecisionTree<mlpack::GiniGain>;
using ArbolEntropia = mlpack::DecisionTree<mlpack::InformationGain>;

// Criterios en el mismo orden que la opcion "criterio"
constexpr int CRITERIO_GINI = 0;
constexpr int CRITERIO_ENTROPIA = 1;

struct ModeloArbol {
    int criterio = CRITERIO_GINI;
    std::unique_ptr<ArbolGini> gini;         // solo uno de los dos existe
    std::unique_ptr<ArbolEntropia> entropia;
    int32_t n_clases = 0;
    int32_t n_cols = 0;
};

energia::RegistroModelos<ModeloArbol> g_modelos;

const std::vector<EnergiaHiperparametro>& descripcion()
{
    static const std::vector<EnergiaHiperparametro> d = {
        energia::hiperOpcion("criterio", "Criterio",
                             "Formula para elegir el mejor corte en cada nodo. Entropia es algo mas costosa de calcular.",
                             "gini;entropia", CRITERIO_GINI),
        energia::hiperEntero("max_depth", "Profundidad maxima",
                             "Niveles del arbol contando la raiz (1 = solo la raiz). 0 = sin limite.",
                             0, 10000, 0),
        energia::hiperEntero("min_hoja", "Minimo de muestras por hoja",
                             "Ninguna hoja final puede quedar con menos filas que esto.",
                             1, 1000000, 10),
        energia::hiperReal("ganancia_min", "Ganancia minima para dividir",
                           "Un nodo solo se divide si el corte mejora al menos esto.",
                           0.0, 1.0, 1e-7),
    };
    return d;
}

// Datos del contrato (filas x columnas, por filas) -> formato de mlpack (una columna por fila)
arma::mat aMatriz(const EnergiaDatos* d)
{
    // El bloque row-major n x c leido como column-major c x n ya es la traspuesta que pide mlpack
    return arma::mat(d->X, static_cast<arma::uword>(d->n_cols), static_cast<arma::uword>(d->n_filas));
}

arma::Row<size_t> aEtiquetas(const EnergiaDatos* d)
{
    arma::Row<size_t> y(static_cast<arma::uword>(d->n_filas));
    for (int64_t i = 0; i < d->n_filas; ++i) y[static_cast<arma::uword>(i)] = static_cast<size_t>(d->Y[i]);
    return y;
}

// Recorre el arbol ya construido: niveles (raiz = 1) y hojas
template <class Arbol>
void medir(const Arbol& nodo, size_t nivel, size_t* profundidad, size_t* hojas)
{
    if (nivel > *profundidad) *profundidad = nivel;
    if (nodo.NumChildren() == 0) { ++*hojas; return; }
    for (size_t i = 0; i < nodo.NumChildren(); ++i) medir(nodo.Child(i), nivel + 1, profundidad, hojas);
}

template <class Arbol>
std::unique_ptr<Arbol> construir(arma::mat datos, arma::Row<size_t> etiquetas, size_t nClases,
                                 size_t minHoja, double gananciaMin, size_t maxDepth,
                                 size_t* profundidad, size_t* hojas)
{
    auto arbol = std::make_unique<Arbol>();
    arbol->Train(std::move(datos), std::move(etiquetas), nClases, minHoja, gananciaMin, maxDepth);
    medir(*arbol, 1, profundidad, hojas);
    return arbol;
}

bool avisar(EnergiaProgressFn cb, void* ud, const EnergiaProgreso& p)
{
    return cb && cb(&p, ud) == ENERGIA_DETENER;
}

std::string textoLibreria()
{
    char buf[ENERGIA_LEN_TEXTO];
    std::snprintf(buf, sizeof(buf), "mlpack %d.%d.%d / Armadillo %d.%d.%d", MLPACK_VERSION_MAJOR,
                  MLPACK_VERSION_MINOR, MLPACK_VERSION_PATCH, ARMA_VERSION_MAJOR, ARMA_VERSION_MINOR,
                  ARMA_VERSION_PATCH);
    return buf;
}

} // namespace

extern "C" ENERGIA_API int32_t energia_api_version(void)
{
    return ENERGIA_API_VERSION;
}

extern "C" ENERGIA_API int32_t energia_info_modelo(EnergiaInfoModelo* info)
{
    try {
        if (!info) return ENERGIA_ERR_PARAMETROS;
        std::memset(info, 0, sizeof(*info));
        energia::copiarTexto(info->clave, sizeof(info->clave), "arbol_decision");
        energia::copiarTexto(info->nombre, sizeof(info->nombre), "Arbol de decision");
        energia::copiarTexto(info->libreria, sizeof(info->libreria), textoLibreria().c_str());
        energia::copiarTexto(info->unidad_paso, sizeof(info->unidad_paso), "arbol");
        info->hilos = 1;
        return ENERGIA_OK;
    } catch (...) {
        return ENERGIA_ERR_INTERNO;
    }
}

extern "C" ENERGIA_API int32_t energia_n_hiperparametros(void)
{
    return static_cast<int32_t>(descripcion().size());
}

extern "C" ENERGIA_API int32_t energia_describir_hiperparametro(int32_t i, EnergiaHiperparametro* out)
{
    const auto& d = descripcion();
    if (!out || i < 0 || i >= static_cast<int32_t>(d.size())) return ENERGIA_ERR_PARAMETROS;
    *out = d[static_cast<size_t>(i)];
    return ENERGIA_OK;
}

extern "C" ENERGIA_API int32_t entrenar(const EnergiaDatos* train, const EnergiaDatos* val,
                                        const EnergiaParams* params, EnergiaProgressFn callback,
                                        void* user_data, int64_t* modelo_id)
{
    try {
        if (modelo_id) *modelo_id = 0;
        if (!params || !modelo_id) return ENERGIA_ERR_PARAMETROS;

        energia::ValoresLeidos v;
        if (!energia::validarParams(*params, descripcion(), &v).empty()) return ENERGIA_ERR_PARAMETROS;
        if (params->n_clases < 2) return ENERGIA_ERR_DATOS;
        if (!energia::datosValidos(train, params->n_clases) || !energia::datosValidos(val, params->n_clases))
            return ENERGIA_ERR_DATOS;
        if (train->n_cols != val->n_cols) return ENERGIA_ERR_DATOS;

        const int criterio = static_cast<int>(v["criterio"].entero());
        const size_t maxDepth = static_cast<size_t>(v["max_depth"].entero());
        const size_t minHoja = static_cast<size_t>(v["min_hoja"].entero());
        const double gananciaMin = v["ganancia_min"].numero;
        const size_t nClases = static_cast<size_t>(params->n_clases);

        EnergiaProgreso p;
        std::memset(&p, 0, sizeof(p));
        p.fase = ENERGIA_FASE_START;
        p.step_index = 0;
        p.step_total = 1;
        if (avisar(callback, user_data, p)) return ENERGIA_CANCELADO;

        // ---- Todo esto es el entrenamiento medido ----
        auto m = std::make_unique<ModeloArbol>();
        m->criterio = criterio;
        m->n_clases = params->n_clases;
        m->n_cols = train->n_cols;
        size_t profundidad = 0, hojas = 0;
        if (criterio == CRITERIO_ENTROPIA)
            m->entropia = construir<ArbolEntropia>(aMatriz(train), aEtiquetas(train), nClases, minHoja,
                                                   gananciaMin, maxDepth, &profundidad, &hojas);
        else
            m->gini = construir<ArbolGini>(aMatriz(train), aEtiquetas(train), nClases, minHoja,
                                           gananciaMin, maxDepth, &profundidad, &hojas);
        // ------------------------------------------------

        std::memset(&p, 0, sizeof(p));
        p.fase = ENERGIA_FASE_END;
        p.step_index = 0;
        p.step_total = 1;
        energia::ponerMetrica(p, "profundidad", static_cast<double>(profundidad));
        energia::ponerMetrica(p, "hojas", static_cast<double>(hojas));
        if (avisar(callback, user_data, p)) return ENERGIA_CANCELADO;  // m se libera solo

        *modelo_id = g_modelos.agregar(std::move(m));
        return ENERGIA_OK;
    } catch (...) {
        if (modelo_id) *modelo_id = 0;
        return ENERGIA_ERR_INTERNO;  // ninguna excepcion cruza la frontera
    }
}

extern "C" ENERGIA_API int32_t evaluar(int64_t modelo_id, const EnergiaDatos* datos, EnergiaMetrica* metricas,
                                       int32_t max_metricas, int32_t* n_metricas)
{
    try {
        if (n_metricas) *n_metricas = 0;
        if (!metricas || !n_metricas || max_metricas <= 0) return ENERGIA_ERR_PARAMETROS;
        const ModeloArbol* m = g_modelos.buscar(modelo_id);
        if (!m) return ENERGIA_ERR_MODELO_INVALIDO;
        if (!energia::datosValidos(datos, m->n_clases) || datos->n_cols != m->n_cols) return ENERGIA_ERR_DATOS;

        const arma::mat X = aMatriz(datos);
        arma::Row<size_t> pred;
        if (m->entropia) m->entropia->Classify(X, pred);
        else m->gini->Classify(X, pred);

        int64_t aciertos = 0;
        for (int64_t i = 0; i < datos->n_filas; ++i)
            if (pred[static_cast<arma::uword>(i)] == static_cast<size_t>(datos->Y[i])) ++aciertos;
        energia::ponerMetrica(metricas, max_metricas, n_metricas, "val_accuracy",
                              static_cast<double>(aciertos) / static_cast<double>(datos->n_filas));
        return ENERGIA_OK;
    } catch (...) {
        return ENERGIA_ERR_INTERNO;
    }
}

// Formato binario de cereal, el mismo que usa mlpack::data::Save con extension .bin.
// Para volver a cargarlo hay que saber el criterio (queda en metadata.json).
extern "C" ENERGIA_API int32_t guardar_modelo(int64_t modelo_id, const char* ruta_utf8)
{
    try {
        if (!ruta_utf8) return ENERGIA_ERR_PARAMETROS;
        const ModeloArbol* m = g_modelos.buscar(modelo_id);
        if (!m) return ENERGIA_ERR_MODELO_INVALIDO;
        std::ofstream f;
        if (!energia::abrirSalida(ruta_utf8, &f)) return ENERGIA_ERR_ARCHIVO;
        {
            cereal::BinaryOutputArchive ar(f);
            if (m->entropia) ar(cereal::make_nvp("model", *m->entropia));
            else ar(cereal::make_nvp("model", *m->gini));
        }
        f.close();
        return f ? ENERGIA_OK : ENERGIA_ERR_ARCHIVO;
    } catch (...) {
        return ENERGIA_ERR_INTERNO;
    }
}

extern "C" ENERGIA_API int32_t liberar_modelo(int64_t modelo_id)
{
    try {
        return g_modelos.quitar(modelo_id) ? ENERGIA_OK : ENERGIA_ERR_MODELO_INVALIDO;
    } catch (...) {
        return ENERGIA_ERR_INTERNO;
    }
}
