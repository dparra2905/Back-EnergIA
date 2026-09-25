// energia_comun.h - Ayudas en C++ comunes a las DLL de modelos y al orquestador.
//
// NO es parte del contrato: nada de esto cruza la frontera de la DLL. Cada DLL y el
// orquestador lo compilan por su lado. Existe para que la validacion de hiperparametros sea
// exactamente la misma en los dos lados (el orquestador valida ANTES de lanzar medidores,
// asi un error no aparece despues de 30 s de margen y estabilizacion) y para no repetir
// codigo en cada DLL.
//
// Solo usa la libreria estandar de C++17 (sin windows.h, sin Qt).
#pragma once

#include "energia_dll.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace energia {

// ------------------------------------------------------------------ textos de tamano fijo
inline void copiarTexto(char* destino, size_t tam, const char* origen)
{
    if (!destino || tam == 0) return;
    if (!origen) origen = "";
    std::strncpy(destino, origen, tam - 1);
    destino[tam - 1] = '\0';
}

// Lee un arreglo char[N] sin confiar en que venga terminado en '\0'
inline std::string leerTexto(const char* arreglo, size_t tam)
{
    size_t n = 0;
    while (n < tam && arreglo[n] != '\0') ++n;
    return std::string(arreglo, n);
}

template <size_t N>
inline std::string leerTexto(const char (&arreglo)[N]) { return leerTexto(arreglo, N); }

// ------------------------------------------------------------------ descripcion de hiperparametros
inline EnergiaHiperparametro hiperBase(int32_t tipo, const char* clave, const char* etiqueta, const char* ayuda)
{
    EnergiaHiperparametro h;
    std::memset(&h, 0, sizeof(h));
    h.tipo = tipo;
    copiarTexto(h.clave, sizeof(h.clave), clave);
    copiarTexto(h.etiqueta, sizeof(h.etiqueta), etiqueta);
    copiarTexto(h.ayuda, sizeof(h.ayuda), ayuda);
    return h;
}

inline EnergiaHiperparametro hiperEntero(const char* clave, const char* etiqueta, const char* ayuda,
                                         int64_t minimo, int64_t maximo, int64_t porDefecto)
{
    EnergiaHiperparametro h = hiperBase(ENERGIA_TIPO_ENTERO, clave, etiqueta, ayuda);
    h.minimo = static_cast<double>(minimo);
    h.maximo = static_cast<double>(maximo);
    h.por_defecto = static_cast<double>(porDefecto);
    return h;
}

inline EnergiaHiperparametro hiperReal(const char* clave, const char* etiqueta, const char* ayuda,
                                       double minimo, double maximo, double porDefecto)
{
    EnergiaHiperparametro h = hiperBase(ENERGIA_TIPO_REAL, clave, etiqueta, ayuda);
    h.minimo = minimo;
    h.maximo = maximo;
    h.por_defecto = porDefecto;
    return h;
}

// opciones separadas por ';' (ej. "gini;entropia"); porDefecto es el indice
inline EnergiaHiperparametro hiperOpcion(const char* clave, const char* etiqueta, const char* ayuda,
                                         const char* opciones, int32_t porDefecto)
{
    EnergiaHiperparametro h = hiperBase(ENERGIA_TIPO_OPCION, clave, etiqueta, ayuda);
    copiarTexto(h.opciones, sizeof(h.opciones), opciones);
    h.por_defecto = porDefecto;
    return h;
}

inline EnergiaHiperparametro hiperLista(const char* clave, const char* etiqueta, const char* ayuda,
                                        int64_t minimo, int64_t maximo, int32_t maxElementos,
                                        const char* porDefecto)
{
    EnergiaHiperparametro h = hiperBase(ENERGIA_TIPO_LISTA_ENTEROS, clave, etiqueta, ayuda);
    h.minimo = static_cast<double>(minimo);
    h.maximo = static_cast<double>(maximo);
    h.max_elementos = maxElementos;
    copiarTexto(h.defecto_texto, sizeof(h.defecto_texto), porDefecto);
    return h;
}

inline std::vector<std::string> separarOpciones(const std::string& opciones)
{
    std::vector<std::string> salida;
    size_t inicio = 0;
    while (inicio <= opciones.size()) {
        const size_t fin = opciones.find(';', inicio);
        const std::string parte = opciones.substr(inicio, fin == std::string::npos ? std::string::npos : fin - inicio);
        if (!parte.empty()) salida.push_back(parte);
        if (fin == std::string::npos) break;
        inicio = fin + 1;
    }
    return salida;
}

// "64, 32" -> {64, 32}. Solo enteros separados por comas; espacios permitidos.
inline bool parsearLista(const std::string& texto, std::vector<int64_t>* salida)
{
    salida->clear();
    size_t i = 0;
    const size_t n = texto.size();
    auto saltarEspacios = [&] { while (i < n && (texto[i] == ' ' || texto[i] == '\t')) ++i; };
    saltarEspacios();
    if (i == n) return false;  // vacio
    while (true) {
        saltarEspacios();
        bool negativo = false;
        if (i < n && (texto[i] == '-' || texto[i] == '+')) { negativo = texto[i] == '-'; ++i; }
        if (i == n || texto[i] < '0' || texto[i] > '9') return false;
        int64_t v = 0;
        while (i < n && texto[i] >= '0' && texto[i] <= '9') {
            if (v > 100000000000LL) return false;  // evita desbordes absurdos
            v = v * 10 + (texto[i] - '0');
            ++i;
        }
        salida->push_back(negativo ? -v : v);
        saltarEspacios();
        if (i == n) return true;
        if (texto[i] != ',') return false;
        ++i;
    }
}

// ------------------------------------------------------------------ validacion de valores
struct ValorLeido {
    int32_t tipo = ENERGIA_TIPO_ENTERO;
    double numero = 0.0;          // ENTERO, REAL, OPCION (indice)
    std::string opcion;           // OPCION: texto de la opcion elegida
    std::vector<int64_t> lista;   // LISTA_ENTEROS
    std::string texto;            // LISTA_ENTEROS tal como llego

    int64_t entero() const { return static_cast<int64_t>(numero); }
};
using ValoresLeidos = std::map<std::string, ValorLeido>;

// Comprueba que params traiga exactamente un valor valido por hiperparametro descrito.
// Devuelve "" si todo esta bien, o el motivo del error. Si sale bien, llena *salida.
inline std::string validarParams(const EnergiaParams& p, const std::vector<EnergiaHiperparametro>& desc,
                                 ValoresLeidos* salida)
{
    ValoresLeidos leidos;
    if (p.n_valores < 0 || p.n_valores > ENERGIA_MAX_HIPER) return "n_valores fuera de rango";
    if (p.n_valores != static_cast<int32_t>(desc.size()))
        return "se esperaban " + std::to_string(desc.size()) + " valores y llegaron " + std::to_string(p.n_valores);

    for (int32_t i = 0; i < p.n_valores; ++i) {
        const EnergiaValor& v = p.valores[i];
        const std::string clave = leerTexto(v.clave);
        const EnergiaHiperparametro* h = nullptr;
        for (const EnergiaHiperparametro& d : desc)
            if (leerTexto(d.clave) == clave) { h = &d; break; }
        if (!h) return "hiperparametro desconocido '" + clave + "'";
        if (leidos.count(clave)) return "hiperparametro repetido '" + clave + "'";

        const std::string etiqueta = leerTexto(h->etiqueta);
        ValorLeido l;
        l.tipo = h->tipo;
        l.numero = v.numero;
        switch (h->tipo) {
        case ENERGIA_TIPO_ENTERO:
            if (!std::isfinite(v.numero) || v.numero != std::floor(v.numero)) return etiqueta + ": debe ser entero";
            if (v.numero < h->minimo || v.numero > h->maximo) return etiqueta + ": fuera de rango";
            break;
        case ENERGIA_TIPO_REAL:
            if (!std::isfinite(v.numero)) return etiqueta + ": no es un numero";
            if (v.numero < h->minimo || v.numero > h->maximo) return etiqueta + ": fuera de rango";
            break;
        case ENERGIA_TIPO_OPCION: {
            const std::vector<std::string> ops = separarOpciones(leerTexto(h->opciones));
            if (v.numero != std::floor(v.numero) || v.numero < 0 || v.numero >= static_cast<double>(ops.size()))
                return etiqueta + ": opcion invalida";
            l.opcion = ops[static_cast<size_t>(v.numero)];
            break;
        }
        case ENERGIA_TIPO_LISTA_ENTEROS: {
            l.texto = leerTexto(v.texto);
            if (!parsearLista(l.texto, &l.lista)) return etiqueta + ": escribe enteros separados por comas (ej. 64,32)";
            if (l.lista.size() > static_cast<size_t>(h->max_elementos))
                return etiqueta + ": maximo " + std::to_string(h->max_elementos) + " numeros";
            for (int64_t x : l.lista)
                if (x < h->minimo || x > h->maximo) return etiqueta + ": el valor " + std::to_string(x) + " esta fuera de rango";
            break;
        }
        default:
            return etiqueta + ": tipo desconocido";
        }
        leidos[clave] = l;
    }
    if (salida) *salida = std::move(leidos);
    return std::string();
}

// ------------------------------------------------------------------ datos y metricas
inline bool datosValidos(const EnergiaDatos* d, int32_t nClases)
{
    if (!d || !d->X || !d->Y || d->n_filas <= 0 || d->n_cols <= 0) return false;
    for (int64_t i = 0; i < d->n_filas; ++i) {
        const double y = d->Y[i];
        if (!(y >= 0) || y >= nClases || y != std::floor(y)) return false;
    }
    return true;
}

inline void ponerMetrica(EnergiaMetrica* metricas, int32_t maximo, int32_t* n, const char* nombre, double valor)
{
    if (!metricas || !n || *n >= maximo) return;
    EnergiaMetrica& m = metricas[(*n)++];
    copiarTexto(m.nombre, sizeof(m.nombre), nombre);
    m.valor = valor;
}

inline void ponerMetrica(EnergiaProgreso& p, const char* nombre, double valor)
{
    ponerMetrica(p.metricas, ENERGIA_MAX_METRICAS, &p.n_metricas, nombre, valor);
}

// ------------------------------------------------------------------ modelos vivos en la DLL
template <class T>
class RegistroModelos {
public:
    int64_t agregar(std::unique_ptr<T> m)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t id = siguiente_++;
        modelos_[id] = std::move(m);
        return id;
    }
    // El orquestador nunca usa un modelo desde dos hilos a la vez, asi que basta el puntero
    T* buscar(int64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = modelos_.find(id);
        return it == modelos_.end() ? nullptr : it->second.get();
    }
    bool quitar(int64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return modelos_.erase(id) > 0;
    }

private:
    std::mutex mutex_;
    std::map<int64_t, std::unique_ptr<T>> modelos_;
    int64_t siguiente_ = 1;
};

// ------------------------------------------------------------------ archivos con ruta UTF-8
// std::filesystem::u8path acepta la ruta UTF-8 aunque tenga tildes o enes, en MSVC y MinGW
inline bool abrirSalida(const char* rutaUtf8, std::ofstream* f)
{
    if (!rutaUtf8 || !f) return false;
    f->open(std::filesystem::u8path(rutaUtf8), std::ios::binary | std::ios::trunc);
    return f->is_open();
}

} // namespace energia
