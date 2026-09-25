// orquestador.cpp - Orquestador de EnergIA (Qt Widgets, un solo archivo).
//
// Linea de tiempo de una corrida:
//
//   [Iniciar] -> preparar datos (CSV o sinteticos, particion) -> lanzar medidores
//     -> margen de conexion -> [t_start] -> estabilizacion pre -> [t_antes | DLL | t_despues]
//     -> estabilizacion post -> stop.flag -> medidores cierran solos
//     -> evaluar en validacion -> guardar modelo (opcional) -> liberarlo -> metadata.json final
//
//   - Modelos: una DLL por modelo (EnergIA_*.dll en la carpeta del ejecutable). Al abrir, el
//     orquestador carga cada una, lee su descripcion (nombre, libreria, hiperparametros), arma
//     la ventana con eso y la descarga. En cada corrida carga solo la DLL elegida y la descarga
//     al terminar, para que un modelo no arrastre la memoria de otro (ej. libtorch).
//
//   - Hilo principal (ventana): coordina, calcula t_start, lanza medidores, controla fases.
//   - Hilo de trabajo (QThread): prepara los datos y hace la llamada bloqueante a la DLL.
//   - Medidores: procesos aislados. Reciben t_start y la carpeta de la corrida por argumentos
//     y se detienen cuando aparece stop.flag en esa carpeta.
//
// Timestamps: reloj de pared UTC, texto ISO con milisegundos (2026-09-22T00:52:45.334Z),
// el mismo formato que usa atorch_ble_capture.py.

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibrary>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "energia_dll.h"
#include "energia_comun.h"

// =====================================================================================
// CONFIGURACION DEL LABORATORIO (cambia poco; lo del experimento se elige en la ventana)
// =====================================================================================
namespace cfg {

// DLL de modelos y carpeta de salida (relativas a la carpeta del ejecutable)
#ifdef Q_OS_WIN
const char* const PATRON_DLL       = "EnergIA_*.dll";  // una DLL por modelo
#else
const char* const PATRON_DLL       = "EnergIA_*.so";
#endif
const char* const CARPETA_CORRIDAS = "corridas";
const char* const ARCHIVO_MODELO   = "modelo.bin";

// Tiempos de la corrida
constexpr qint64 MARGEN_CONEXION_MS         = 10000;  // medidores se conectan antes de t_start
constexpr qint64 ESTABILIZACION_PRE_MS      = 20000;  // linea base antes de la DLL
constexpr qint64 ESTABILIZACION_POST_MS     = 20000;  // linea base despues de la DLL
constexpr qint64 ESPERA_CIERRE_MEDIDORES_MS = 15000;  // tras stop.flag, antes de forzar cierre
constexpr int    ESPERA_ARRANQUE_MEDIDOR_MS = 5000;   // maximo para que un medidor arranque
constexpr int    DURACION_MAXIMA_MEDIDORES_S = 4 * 3600;  // tope de seguridad (--duration)

// Datos
constexpr int MAX_CLASES = 100;  // mas valores distintos que esto: probablemente es regresion

// Datos sinteticos para pruebas
constexpr int SINTETICO_FILAS  = 1500;
constexpr int SINTETICO_COLS   = 8;
constexpr int SINTETICO_CLASES = 3;

// Valores iniciales de la ventana (los de cada modelo los define su DLL)
constexpr int DEF_PORCENTAJE_TRAIN = 80;
constexpr int DEF_SEMILLA          = 42;

// Medidores: programa + argumentos. Marcadores que el orquestador reemplaza en cada corrida:
//   {T_START_UTC}      t_start en ISO UTC con milisegundos
//   {CARPETA_CORRIDA}  carpeta de la corrida (ahi aparece stop.flag)
//   {DURACION_MAX_S}   tope de seguridad en segundos
//   {PID_ORQUESTADOR}  PID de este proceso
// Si "programa" es un nombre relativo y existe en la carpeta del ejecutable, se usa esa
// copia (ej. system_meter.exe); si no, se busca en el PATH (ej. python).
// archivoListo: si no esta vacio, el medidor debe crear ese archivo en la carpeta de la
// corrida cuando ya esta midiendo (ej. energia_lista.flag con la primera trama del Atorch).
// Al llegar t_start, si falta, la corrida se aborta antes de entrenar.
struct Medidor {
    QString nombre;
    QString programa;
    QStringList argumentos;
    QString archivoListo;
};

// Carpeta medidores/ del proyecto (energia_ble.py y su entorno virtual venv/).
// CMake la define al compilar; no hace falta escribir rutas a mano.
#ifndef ENERGIA_DIR_MEDIDORES
#define ENERGIA_DIR_MEDIDORES "medidores"
#endif

inline QString dirMedidores() { return QString::fromUtf8(ENERGIA_DIR_MEDIDORES); }

inline QString pythonBle()
{
#ifdef Q_OS_WIN
    return dirMedidores() + "/venv/Scripts/python.exe";
#else
    return dirMedidores() + "/venv/bin/python";
#endif
}

inline QString scriptBle() { return dirMedidores() + "/energia_ble.py"; }

inline QList<Medidor> medidores()
{
    return {
        // Medidor de sistema (CPU/RAM): system_meter.exe, compilado junto al orquestador
        {"sistema", "system_meter.exe",
         {"--t-start", "{T_START_UTC}", "--carpeta", "{CARPETA_CORRIDA}",
          "--duration", "{DURACION_MAX_S}", "--pid", "{PID_ORQUESTADOR}"},
         ""},

        // Medidor de energia (Atorch S1BW por Bluetooth): energia_ble.py
        {"energia", pythonBle(),
         {scriptBle(), "medir",
          "--t-start-utc", "{T_START_UTC}", "--carpeta", "{CARPETA_CORRIDA}",
          "--duracion-max-s", "{DURACION_MAX_S}", "--pid", "{PID_ORQUESTADOR}"},
         "energia_lista.flag"},
    };
}

} // namespace cfg

// =====================================================================================
// RELOJ COMUN Y FORMATO DE TIEMPO
// =====================================================================================
static qint64 ahoraMs()
{
#ifdef Q_OS_WIN
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<qint64>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
#else
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
#endif
}

// Milisegundos desde epoch -> "2026-09-22T00:52:45.334Z" (UTC)
static QByteArray isoUtc(qint64 ms)
{
    qint64 dias = ms / 86400000;
    qint64 resto = ms % 86400000;
    if (resto < 0) { resto += 86400000; --dias; }

    // Conversion de dias desde 1970-01-01 a fecha civil (algoritmo de H. Hinnant)
    const qint64 z = dias + 719468;
    const qint64 era = (z >= 0 ? z : z - 146096) / 146097;
    const qint64 doe = z - era * 146097;
    const qint64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const qint64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const qint64 mp = (5 * doy + 2) / 153;
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int y = static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0));

    const int h = static_cast<int>(resto / 3600000);
    const int mi = static_cast<int>((resto / 60000) % 60);
    const int s = static_cast<int>((resto / 1000) % 60);
    const int mil = static_cast<int>(resto % 1000);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", y, m, d, h, mi, s, mil);
    return QByteArray(buf);
}

static int msHasta(qint64 objetivoMs)
{
    const qint64 ms = objetivoMs - ahoraMs();
    return ms > 0 ? static_cast<int>(ms) : 0;
}

static QString describirEstado(int st)
{
    switch (st) {
    case ENERGIA_OK:                  return "ok";
    case ENERGIA_ERR_PARAMETROS:      return "parametros invalidos";
    case ENERGIA_ERR_DATOS:           return "datos invalidos";
    case ENERGIA_ERR_INTERNO:         return "error interno de la DLL";
    case ENERGIA_CANCELADO:           return "cancelado";
    case ENERGIA_ERR_MODELO_INVALIDO: return "identificador de modelo invalido";
    case ENERGIA_ERR_ARCHIVO:         return "no se pudo escribir el archivo";
    default:                          return QString("codigo desconocido %1").arg(st);
    }
}

// =====================================================================================
// LECTURA DE CSV (comun a la ventana y al hilo de trabajo)
// =====================================================================================
static char detectarSeparador(const QByteArray& encabezado)
{
    const int comas = encabezado.count(',');
    const int puntoYComa = encabezado.count(';');
    const int tabs = encabezado.count('\t');
    if (puntoYComa > comas && puntoYComa >= tabs) return ';';
    if (tabs > comas && tabs > puntoYComa) return '\t';
    return ',';
}

static QByteArray limpiarCampo(QByteArray campo)
{
    campo = campo.trimmed();
    if (campo.size() >= 2 && campo.startsWith('"') && campo.endsWith('"')) campo = campo.mid(1, campo.size() - 2).trimmed();
    return campo;
}

// Lee solo la primera linea: nombres de columna y separador
static bool leerEncabezado(const QString& ruta, QStringList* columnas, char* separador, QString* error)
{
    QFile f(ruta);
    if (!f.open(QIODevice::ReadOnly)) { *error = "No se pudo abrir " + ruta; return false; }
    QByteArray linea = f.readLine();
    if (linea.startsWith("\xEF\xBB\xBF")) linea = linea.mid(3);  // BOM de Excel
    linea = linea.trimmed();
    if (linea.isEmpty()) { *error = "El CSV no tiene encabezado"; return false; }

    *separador = detectarSeparador(linea);
    columnas->clear();
    for (const QByteArray& c : linea.split(*separador)) columnas->append(QString::fromUtf8(limpiarCampo(c)));
    if (columnas->size() < 2) { *error = "El CSV necesita al menos 2 columnas"; return false; }
    return true;
}

// =====================================================================================
// HILO DE TRABAJO: prepara datos, llama a la DLL, evalua, guarda y libera el modelo
// =====================================================================================
using FnVersion   = int32_t (*)();
using FnInfo      = int32_t (*)(EnergiaInfoModelo*);
using FnNHiper    = int32_t (*)();
using FnDescribir = int32_t (*)(int32_t, EnergiaHiperparametro*);
using FnEntrenar  = int32_t (*)(const EnergiaDatos*, const EnergiaDatos*, const EnergiaParams*,
                                EnergiaProgressFn, void*, int64_t*);
using FnEvaluar   = int32_t (*)(int64_t, const EnergiaDatos*, EnergiaMetrica*, int32_t, int32_t*);
using FnGuardar   = int32_t (*)(int64_t, const char*);
using FnLiberar   = int32_t (*)(int64_t);

// Funciones de la DLL cargada para la corrida en curso
struct FuncionesDll {
    FnEntrenar entrenar = nullptr;
    FnEvaluar evaluar = nullptr;
    FnGuardar guardar = nullptr;
    FnLiberar liberar = nullptr;
};

struct ConfigCorrida {
    bool usarSintetico = true;
    QString rutaCsv;
    int columnaObjetivo = -1;
    int porcentajeTrain = 80;
    uint32_t semilla = 42;
    EnergiaParams params{};
    FuncionesDll fn;
    QString rutaProgreso;
};

class Trabajador : public QObject {
    Q_OBJECT
public:

    // Se puede llamar desde el hilo de la ventana: solo toca una variable atomica
    void solicitarCancelacion() { cancelar_.store(true); }

    void preparar(const ConfigCorrida& c)
    {
        cancelar_.store(false);
        listo_ = false;
        cfg_ = c;
        liberarDatos();

        QString error;
        QJsonObject resumen;
        std::vector<double> X, Y;
        int nCols = 0;

        const bool ok = c.usarSintetico ? generarSinteticos(&X, &Y, &nCols, &resumen)
                                        : cargarCsv(c, &X, &Y, &nCols, &resumen, &error);
        if (!ok) { emit preparado(false, error, QJsonObject()); return; }

        // Etiquetas originales -> indices 0 .. n_clases-1
        std::vector<double> clases(Y);
        std::sort(clases.begin(), clases.end());
        clases.erase(std::unique(clases.begin(), clases.end()), clases.end());
        if (clases.size() < 2) { emit preparado(false, "La columna objetivo tiene una sola clase", QJsonObject()); return; }
        if (static_cast<int>(clases.size()) > cfg::MAX_CLASES) {
            emit preparado(false, QString("La columna objetivo tiene %1 valores distintos (maximo %2); "
                                          "parece un problema de regresion").arg(clases.size()).arg(cfg::MAX_CLASES),
                           QJsonObject());
            return;
        }
        for (double& y : Y) y = static_cast<double>(std::lower_bound(clases.begin(), clases.end(), y) - clases.begin());

        // Particion reproducible: Fisher-Yates con mt19937 (mismo resultado en MinGW y MSVC)
        const int64_t n = static_cast<int64_t>(Y.size());
        if (n < 2) { emit preparado(false, "Se necesitan al menos 2 filas", QJsonObject()); return; }
        std::vector<int64_t> idx(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) idx[i] = i;
        std::mt19937 rng(c.semilla);
        for (int64_t i = n - 1; i > 0; --i) std::swap(idx[i], idx[rng() % static_cast<uint32_t>(i + 1)]);

        int64_t nTrain = (n * c.porcentajeTrain + 50) / 100;
        nTrain = std::max<int64_t>(1, std::min<int64_t>(n - 1, nTrain));

        auto copiar = [&](int64_t desde, int64_t hasta, std::vector<double>* xd, std::vector<double>* yd) {
            xd->reserve(static_cast<size_t>((hasta - desde) * nCols));
            yd->reserve(static_cast<size_t>(hasta - desde));
            for (int64_t k = desde; k < hasta; ++k) {
                const int64_t fila = idx[k];
                xd->insert(xd->end(), X.begin() + fila * nCols, X.begin() + (fila + 1) * nCols);
                yd->push_back(Y[fila]);
            }
        };
        copiar(0, nTrain, &Xtrain_, &Ytrain_);
        copiar(nTrain, n, &Xval_, &Yval_);
        nCols_ = nCols;
        nClases_ = static_cast<int32_t>(clases.size());

        // progreso.csv (formato ancho: una fila por paso)
        if (archivo_.isOpen()) archivo_.close();
        archivo_.setFileName(c.rutaProgreso);
        if (!archivo_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit preparado(false, "No se pudo crear " + c.rutaProgreso, QJsonObject());
            return;
        }
        QByteArray enc = "timestamp_inicio,timestamp_fin,step_index,step_total";
        for (int i = 1; i <= ENERGIA_MAX_METRICAS; ++i) enc += QString(",metrica_%1_nombre,metrica_%1_valor").arg(i).toUtf8();
        archivo_.write(enc + '\n');
        archivo_.flush();

        QJsonArray jClases;
        for (double v : clases) jClases.append(v);
        resumen["n_filas_total"] = static_cast<qint64>(n);
        resumen["n_cols"] = nCols;
        resumen["n_train"] = static_cast<qint64>(nTrain);
        resumen["n_val"] = static_cast<qint64>(n - nTrain);
        resumen["porcentaje_train"] = c.porcentajeTrain;
        resumen["semilla"] = static_cast<qint64>(c.semilla);
        resumen["n_clases"] = nClases_;
        resumen["clases_originales"] = jClases;  // la posicion en la lista es el indice 0..n-1

        listo_ = true;
        emit preparado(true, QString(), resumen);
    }

    void ejecutar()
    {
        if (!listo_) { emit terminado(ENERGIA_ERR_INTERNO, 0, 0, 0); return; }

        EnergiaDatos train{Xtrain_.data(), Ytrain_.data(), static_cast<int64_t>(Ytrain_.size()), nCols_};
        EnergiaDatos val{Xval_.data(), Yval_.data(), static_cast<int64_t>(Yval_.size()), nCols_};
        EnergiaParams params = cfg_.params;
        params.n_clases = nClases_;
        params.semilla = cfg_.semilla;
        int64_t modeloId = 0;
        tInicioPaso_ = 0;

        // ---- Frontera temporal: nada entre las marcas y la llamada ----
        const qint64 tAntes = ahoraMs();
        const int32_t status = cfg_.fn.entrenar(&train, &val, &params, &Trabajador::callback, this, &modeloId);
        const qint64 tDespues = ahoraMs();
        // ---------------------------------------------------------------

        archivo_.close();
        listo_ = false;
        emit terminado(status, tAntes, tDespues, static_cast<qint64>(modeloId));
    }

    // Se llama con los medidores ya detenidos: nada de esto entra en la medicion.
    // Orden: evaluar en validacion -> guardar (si se pidio) -> liberar -> soltar los datos.
    void cerrarModelo(qint64 modeloId, bool evaluar, bool guardar, const QString& ruta)
    {
        if (archivo_.isOpen()) archivo_.close();  // corrida cancelada antes de entrenar
        int stGuardar = -1;
        int stLiberar = -1;
        QJsonObject evaluacion;
        evaluacion["ejecutada"] = false;
        if (modeloId > 0) {
            if (evaluar && cfg_.fn.evaluar) {
                EnergiaDatos val{Xval_.data(), Yval_.data(), static_cast<int64_t>(Yval_.size()), nCols_};
                EnergiaMetrica metricas[ENERGIA_MAX_METRICAS];
                std::memset(metricas, 0, sizeof(metricas));
                int32_t n = 0;
                const qint64 t0 = ahoraMs();
                const int32_t st = cfg_.fn.evaluar(modeloId, &val, metricas, ENERGIA_MAX_METRICAS, &n);
                const qint64 t1 = ahoraMs();
                QJsonObject jm;
                n = std::max(0, std::min<int32_t>(n, ENERGIA_MAX_METRICAS));
                for (int i = 0; i < n; ++i)
                    jm[QString::fromStdString(energia::leerTexto(metricas[i].nombre))] = metricas[i].valor;
                evaluacion["ejecutada"] = true;
                evaluacion["status"] = st;
                evaluacion["datos"] = "validacion";
                evaluacion["n_filas"] = static_cast<qint64>(Yval_.size());
                evaluacion["duracion_ms"] = t1 - t0;
                evaluacion["metricas"] = jm;
            }
            if (guardar) stGuardar = cfg_.fn.guardar(modeloId, ruta.toUtf8().constData());
            stLiberar = cfg_.fn.liberar(modeloId);
        }
        liberarDatos();
        emit modeloCerrado(stGuardar, stLiberar, evaluacion);
    }

signals:
    void preparado(bool ok, const QString& error, const QJsonObject& resumen);
    void progreso(int fase, int stepIndex, int stepTotal, const QString& metricas);
    void terminado(int status, qint64 tAntesMs, qint64 tDespuesMs, qint64 modeloId);
    void modeloCerrado(int statusGuardar, int statusLiberar, const QJsonObject& evaluacion);

private:
    static int32_t callback(const EnergiaProgreso* p, void* userData)
    {
        if (!p || !userData) return ENERGIA_CONTINUAR;
        return static_cast<Trabajador*>(userData)->alProgreso(p);
    }

    int32_t alProgreso(const EnergiaProgreso* p)
    {
        const qint64 ts = ahoraMs();  // primero el tiempo, despues todo lo demas

        QString texto;
        if (p->fase == ENERGIA_FASE_START) {
            tInicioPaso_ = ts;
        } else if (p->fase == ENERGIA_FASE_END) {
            QByteArray linea = isoUtc(tInicioPaso_) + ',' + isoUtc(ts) + ','
                             + QByteArray::number(p->step_index) + ',' + QByteArray::number(p->step_total);
            const int n = std::max(0, std::min<int>(p->n_metricas, ENERGIA_MAX_METRICAS));
            for (int i = 0; i < ENERGIA_MAX_METRICAS; ++i) {
                if (i < n) {
                    char nombre[ENERGIA_LEN_NOMBRE + 1];
                    std::memcpy(nombre, p->metricas[i].nombre, ENERGIA_LEN_NOMBRE);
                    nombre[ENERGIA_LEN_NOMBRE] = '\0';  // defensa si la DLL no termino la cadena
                    QByteArray nom(nombre);
                    nom.replace(',', '_').replace('\n', '_').replace('\r', '_');
                    const QByteArray val = QByteArray::number(p->metricas[i].valor, 'g', 12);
                    linea += ',' + nom + ',' + val;
                    texto += QString("  %1=%2").arg(QString::fromUtf8(nom), QString::fromUtf8(val));
                } else {
                    linea += ",,";
                }
            }
            archivo_.write(linea + '\n');
            archivo_.flush();
        }
        emit progreso(p->fase, p->step_index, p->step_total, texto);
        return cancelar_.load() ? ENERGIA_DETENER : ENERGIA_CONTINUAR;
    }

    bool generarSinteticos(std::vector<double>* X, std::vector<double>* Y, int* nCols, QJsonObject* resumen)
    {
        const int n = cfg::SINTETICO_FILAS, c = cfg::SINTETICO_COLS, k = cfg::SINTETICO_CLASES;
        std::mt19937 rng(cfg_.semilla);
        auto unif = [&rng] { return rng() / 4294967296.0; };  // [0, 1), igual en todo compilador
        X->resize(static_cast<size_t>(n) * c);
        Y->resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const int clase = i % k;
            (*Y)[i] = clase;
            for (int j = 0; j < c; ++j) (*X)[static_cast<size_t>(i) * c + j] = clase * 0.5 + (j % 3) * 0.2 + unif();
        }
        *nCols = c;
        (*resumen)["origen"] = "sintetico";
        (*resumen)["columna_objetivo"] = "clase";
        return true;
    }

    bool cargarCsv(const ConfigCorrida& c, std::vector<double>* X, std::vector<double>* Y, int* nCols,
                   QJsonObject* resumen, QString* error)
    {
        QStringList columnas;
        char sep = ',';
        if (!leerEncabezado(c.rutaCsv, &columnas, &sep, error)) return false;
        const int nTotal = columnas.size();
        if (c.columnaObjetivo < 0 || c.columnaObjetivo >= nTotal) { *error = "Columna objetivo invalida"; return false; }

        QFile f(c.rutaCsv);
        if (!f.open(QIODevice::ReadOnly)) { *error = "No se pudo abrir " + c.rutaCsv; return false; }
        f.readLine();  // encabezado

        const bool comaDecimal = (sep != ',');
        std::vector<double> fila(static_cast<size_t>(nTotal));
        int numLinea = 1;
        while (!f.atEnd()) {
            const QByteArray linea = f.readLine().trimmed();
            ++numLinea;
            if (linea.isEmpty()) continue;
            const QList<QByteArray> campos = linea.split(sep);
            if (campos.size() != nTotal) {
                *error = QString("Linea %1: tiene %2 columnas y el encabezado tiene %3").arg(numLinea).arg(campos.size()).arg(nTotal);
                return false;
            }
            for (int j = 0; j < nTotal; ++j) {
                QByteArray v = limpiarCampo(campos[j]);
                if (comaDecimal) v.replace(',', '.');
                bool ok = false;
                fila[j] = v.toDouble(&ok);  // independiente de la configuracion regional
                if (!ok) {
                    *error = QString("Linea %1, columna '%2': el valor '%3' no es numerico")
                                 .arg(numLinea).arg(columnas[j], QString::fromUtf8(campos[j].trimmed()));
                    return false;
                }
            }
            for (int j = 0; j < nTotal; ++j) {
                if (j == c.columnaObjetivo) Y->push_back(fila[j]);
                else X->push_back(fila[j]);
            }
        }
        if (Y->empty()) { *error = "El CSV no tiene filas de datos"; return false; }

        *nCols = nTotal - 1;
        (*resumen)["origen"] = QDir::toNativeSeparators(c.rutaCsv);
        (*resumen)["columna_objetivo"] = columnas[c.columnaObjetivo];
        (*resumen)["separador"] = (sep == '\t') ? QString("tab") : QString(QChar(sep));
        return true;
    }

    void liberarDatos()
    {
        std::vector<double>().swap(Xtrain_);
        std::vector<double>().swap(Ytrain_);
        std::vector<double>().swap(Xval_);
        std::vector<double>().swap(Yval_);
    }

    ConfigCorrida cfg_;
    QFile archivo_;
    std::vector<double> Xtrain_, Ytrain_, Xval_, Yval_;
    int32_t nCols_ = 0;
    int32_t nClases_ = 0;
    qint64 tInicioPaso_ = 0;
    bool listo_ = false;
    std::atomic<bool> cancelar_{false};
};

// =====================================================================================
// HILO PRINCIPAL: ventana y coordinacion de la corrida
// =====================================================================================
class VentanaPrincipal : public QWidget {
    Q_OBJECT
public:
    VentanaPrincipal()
    {
        setWindowTitle("EnergIA - Orquestador");
        resize(900, 700);
        construirInterfaz();

        for (QTimer* t : {&timerPre_, &timerEntrenar_, &timerPost_, &timerForzar_}) {
            t->setSingleShot(true);
            t->setTimerType(Qt::PreciseTimer);
        }
        connect(&timerPre_, &QTimer::timeout, this, &VentanaPrincipal::alLlegarTStart);
        connect(&timerEntrenar_, &QTimer::timeout, this, &VentanaPrincipal::lanzarEntrenamiento);
        connect(&timerPost_, &QTimer::timeout, this, &VentanaPrincipal::detenerMedidores);
        connect(&timerForzar_, &QTimer::timeout, this, &VentanaPrincipal::forzarCierreMedidores);

        // El hilo de trabajo vive toda la sesion; la DLL cambia en cada corrida
        trabajador_ = new Trabajador;
        trabajador_->moveToThread(&hilo_);
        connect(&hilo_, &QThread::finished, trabajador_, &QObject::deleteLater);
        connect(trabajador_, &Trabajador::preparado, this, &VentanaPrincipal::alPreparado);
        connect(trabajador_, &Trabajador::progreso, this, &VentanaPrincipal::alProgreso);
        connect(trabajador_, &Trabajador::terminado, this, &VentanaPrincipal::alTerminar);
        connect(trabajador_, &Trabajador::modeloCerrado, this, &VentanaPrincipal::alCerrarModelo);
        hilo_.start();

        buscarModelos();
    }

    ~VentanaPrincipal() override
    {
        hilo_.quit();
        hilo_.wait();
        if (dll_.isLoaded()) dll_.unload();
    }

protected:
    void closeEvent(QCloseEvent* e) override
    {
        if (corriendo_) {
            log("Hay una corrida en curso; cancelala o espera a que termine antes de cerrar.");
            e->ignore();
            return;
        }
        e->accept();
    }

private:
    struct EstadoMedidor {
        QString nombre;
        QString programa;
        QProcess* proceso = nullptr;
        QString archivoListo;
        int listoEnTStart = -1;  // -1 no aplica, 0 no estaba listo, 1 listo
        bool terminoAntes = false;
        bool forzado = false;
        int codigoSalida = -1;
    };

    // Un modelo encontrado en la carpeta del ejecutable (una DLL)
    struct ModeloDisponible {
        QString ruta;
        EnergiaInfoModelo info{};
        std::vector<EnergiaHiperparametro> hiper;
        QList<QWidget*> editores;  // uno por hiperparametro, en el mismo orden
    };

    // ---------------------------------------------------------------- interfaz
    void construirInterfaz()
    {
        auto* raiz = new QVBoxLayout(this);

        // Experimento
        auto* gExp = new QGroupBox("Experimento", this);
        auto* fExp = new QFormLayout(gExp);
        edExperimento_ = new QLineEdit("prueba", gExp);
        chkGuardar_ = new QCheckBox("Guardar el modelo al terminar", gExp);
        fExp->addRow("Nombre:", edExperimento_);
        fExp->addRow("", chkGuardar_);

        // Datos
        auto* gDatos = new QGroupBox("Datos", this);
        auto* fDatos = new QFormLayout(gDatos);
        chkSintetico_ = new QCheckBox("Usar datos sinteticos (prueba)", gDatos);
        btnCsv_ = new QPushButton("Elegir CSV...", gDatos);
        lblCsv_ = new QLabel("(ninguno)", gDatos);
        lblCsv_->setWordWrap(true);
        cmbObjetivo_ = new QComboBox(gDatos);
        spPorcentaje_ = new QSpinBox(gDatos);
        spPorcentaje_->setRange(50, 95);
        spPorcentaje_->setSuffix(" %");
        spPorcentaje_->setValue(cfg::DEF_PORCENTAJE_TRAIN);
        spSemilla_ = new QSpinBox(gDatos);
        spSemilla_->setRange(0, 2147483647);
        spSemilla_->setValue(cfg::DEF_SEMILLA);
        fDatos->addRow("", chkSintetico_);
        fDatos->addRow(btnCsv_, lblCsv_);
        fDatos->addRow("Columna objetivo:", cmbObjetivo_);
        fDatos->addRow("Entrenamiento:", spPorcentaje_);
        fDatos->addRow("Semilla:", spSemilla_);

        // Modelo: la lista y los campos salen de las DLL que haya en la carpeta
        auto* gModelo = new QGroupBox("Modelo", this);
        auto* vModelo = new QVBoxLayout(gModelo);
        auto* filaModelo = new QHBoxLayout;
        cmbModelo_ = new QComboBox(gModelo);
        btnBuscar_ = new QPushButton("Buscar modelos", gModelo);
        btnBuscar_->setToolTip("Vuelve a leer las DLL EnergIA_* de la carpeta del orquestador");
        filaModelo->addWidget(cmbModelo_, 1);
        filaModelo->addWidget(btnBuscar_);
        lblInfoModelo_ = new QLabel(gModelo);
        lblInfoModelo_->setWordWrap(true);
        pilaParams_ = new QStackedWidget(gModelo);
        vModelo->addLayout(filaModelo);
        vModelo->addWidget(lblInfoModelo_);
        vModelo->addWidget(pilaParams_);
        vModelo->addStretch();

        auto* filaConfig = new QHBoxLayout;
        filaConfig->addWidget(gExp);
        filaConfig->addWidget(gDatos);
        filaConfig->addWidget(gModelo);
        raiz->addLayout(filaConfig);

        // Ejecucion
        auto* filaBotones = new QHBoxLayout;
        btnIniciar_ = new QPushButton("Iniciar corrida", this);
        btnCancelar_ = new QPushButton("Cancelar", this);
        btnCancelar_->setEnabled(false);
        filaBotones->addWidget(btnIniciar_);
        filaBotones->addWidget(btnCancelar_);
        raiz->addLayout(filaBotones);

        lblFase_ = new QLabel("Lista", this);
        QFont fuente = lblFase_->font();
        fuente.setPointSize(fuente.pointSize() + 4);
        fuente.setBold(true);
        lblFase_->setFont(fuente);
        raiz->addWidget(lblFase_);

        barra_ = new QProgressBar(this);
        raiz->addWidget(barra_);

        log_ = new QPlainTextEdit(this);
        log_->setReadOnly(true);
        raiz->addWidget(log_, 1);

        panelesConfig_ = {gExp, gDatos, gModelo};

        connect(cmbModelo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &VentanaPrincipal::alCambiarModelo);
        connect(btnBuscar_, &QPushButton::clicked, this, &VentanaPrincipal::buscarModelos);
        connect(chkSintetico_, &QCheckBox::toggled, this, &VentanaPrincipal::actualizarModoDatos);
        connect(btnCsv_, &QPushButton::clicked, this, &VentanaPrincipal::elegirCsv);
        connect(btnIniciar_, &QPushButton::clicked, this, &VentanaPrincipal::iniciarCorrida);
        connect(btnCancelar_, &QPushButton::clicked, this, &VentanaPrincipal::cancelar);

        chkSintetico_->setChecked(true);
        actualizarModoDatos(true);
    }

    // ---------------------------------------------------------------- modelos (DLL)
    // Busca EnergIA_* en la carpeta del ejecutable, lee la descripcion de cada una y la descarga
    void buscarModelos()
    {
        const QString claveAnterior = modeloActual() ? QString::fromStdString(energia::leerTexto(modeloActual()->info.clave)) : QString();

        cmbModelo_->blockSignals(true);
        cmbModelo_->clear();
        while (pilaParams_->count() > 0) {
            QWidget* w = pilaParams_->widget(0);
            pilaParams_->removeWidget(w);
            delete w;
        }
        modelos_.clear();

        const QDir dir(QCoreApplication::applicationDirPath());
        const QStringList archivos = dir.entryList({QString::fromLatin1(cfg::PATRON_DLL)}, QDir::Files, QDir::Name);
        for (const QString& archivo : archivos) {
            ModeloDisponible m;
            m.ruta = dir.filePath(archivo);
            QString error;
            if (!leerDescripcion(&m, &error)) {
                log(QString("Modelo ignorado (%1): %2").arg(archivo, error));
                continue;
            }
            const QString clave = QString::fromStdString(energia::leerTexto(m.info.clave));
            bool repetida = false;
            for (const ModeloDisponible& otro : modelos_)
                if (QString::fromStdString(energia::leerTexto(otro.info.clave)) == clave) repetida = true;
            if (repetida) { log(QString("Modelo ignorado (%1): clave '%2' repetida").arg(archivo, clave)); continue; }

            pilaParams_->addWidget(crearPaginaParams(&m));
            cmbModelo_->addItem(QString::fromStdString(energia::leerTexto(m.info.nombre)));
            log(QString("Modelo disponible: %1 [%2, %3]").arg(QString::fromStdString(energia::leerTexto(m.info.nombre)),
                                                              QString::fromStdString(energia::leerTexto(m.info.libreria)), archivo));
            modelos_.append(m);
        }

        int indice = 0;
        for (int i = 0; i < modelos_.size(); ++i)
            if (QString::fromStdString(energia::leerTexto(modelos_[i].info.clave)) == claveAnterior) indice = i;
        cmbModelo_->blockSignals(false);
        if (modelos_.isEmpty()) {
            log(QString("No hay modelos: no se encontro ninguna DLL %1 valida en %2")
                    .arg(cfg::PATRON_DLL, QDir::toNativeSeparators(dir.absolutePath())));
            lblInfoModelo_->setText("(sin modelos)");
            return;
        }
        cmbModelo_->setCurrentIndex(indice);
        alCambiarModelo(indice);
    }

    // Carga la DLL, verifica el contrato, copia su descripcion y la descarga
    bool leerDescripcion(ModeloDisponible* m, QString* error)
    {
        QLibrary lib(m->ruta);
        if (!lib.load()) { *error = "no se pudo cargar: " + lib.errorString(); return false; }
        auto fnVersion = reinterpret_cast<FnVersion>(lib.resolve("energia_api_version"));
        auto fnInfo = reinterpret_cast<FnInfo>(lib.resolve("energia_info_modelo"));
        auto fnN = reinterpret_cast<FnNHiper>(lib.resolve("energia_n_hiperparametros"));
        auto fnDesc = reinterpret_cast<FnDescribir>(lib.resolve("energia_describir_hiperparametro"));
        const bool completa = fnVersion && fnInfo && fnN && fnDesc && lib.resolve("entrenar") && lib.resolve("evaluar")
                              && lib.resolve("guardar_modelo") && lib.resolve("liberar_modelo");
        bool ok = false;
        if (!completa) {
            *error = (fnVersion && fnVersion() != ENERGIA_API_VERSION)
                         ? QString("contrato version %1, el orquestador usa la %2").arg(fnVersion()).arg(ENERGIA_API_VERSION)
                         : QString("no exporta todas las funciones del contrato v%1").arg(ENERGIA_API_VERSION);
        } else if (fnVersion() != ENERGIA_API_VERSION) {
            *error = QString("contrato version %1, el orquestador usa la %2").arg(fnVersion()).arg(ENERGIA_API_VERSION);
        } else if (fnInfo(&m->info) != ENERGIA_OK || energia::leerTexto(m->info.clave).empty()) {
            *error = "energia_info_modelo fallo o no dio una clave";
        } else {
            const int32_t n = fnN();
            if (n < 0 || n > ENERGIA_MAX_HIPER) {
                *error = QString("declara %1 hiperparametros (maximo %2)").arg(n).arg(ENERGIA_MAX_HIPER);
            } else {
                ok = true;
                for (int32_t i = 0; i < n && ok; ++i) {
                    EnergiaHiperparametro h;
                    std::memset(&h, 0, sizeof(h));
                    if (fnDesc(i, &h) != ENERGIA_OK) { *error = QString("no describe el hiperparametro %1").arg(i); ok = false; }
                    else m->hiper.push_back(h);
                }
                // Los valores por defecto deben pasar la misma validacion que usara la DLL
                if (ok) {
                    EnergiaParams p;
                    valoresPorDefecto(*m, &p);
                    const std::string motivo = energia::validarParams(p, m->hiper, nullptr);
                    if (!motivo.empty()) { *error = "descripcion invalida: " + QString::fromStdString(motivo); ok = false; }
                }
            }
        }
        lib.unload();
        return ok;
    }

    static void valoresPorDefecto(const ModeloDisponible& m, EnergiaParams* p)
    {
        std::memset(p, 0, sizeof(*p));
        p->n_valores = static_cast<int32_t>(m.hiper.size());
        for (size_t i = 0; i < m.hiper.size(); ++i) {
            const EnergiaHiperparametro& h = m.hiper[i];
            EnergiaValor& v = p->valores[i];
            std::memcpy(v.clave, h.clave, sizeof(v.clave));
            v.numero = h.por_defecto;
            std::memcpy(v.texto, h.defecto_texto, sizeof(v.texto));
        }
    }

    // Un campo por hiperparametro, segun su tipo
    QWidget* crearPaginaParams(ModeloDisponible* m)
    {
        auto* pagina = new QWidget(pilaParams_);
        auto* form = new QFormLayout(pagina);
        form->setContentsMargins(0, 0, 0, 0);
        for (const EnergiaHiperparametro& h : m->hiper) {
            QWidget* editor = nullptr;
            switch (h.tipo) {
            case ENERGIA_TIPO_ENTERO: {
                auto* s = new QSpinBox(pagina);
                s->setRange(static_cast<int>(std::max<double>(h.minimo, INT_MIN)), static_cast<int>(std::min<double>(h.maximo, INT_MAX)));
                s->setValue(static_cast<int>(h.por_defecto));
                editor = s;
                break;
            }
            case ENERGIA_TIPO_REAL: {
                auto* s = new QDoubleSpinBox(pagina);
                // Decimales suficientes para valores pequenos (ej. 1e-7); si no, se redondean a 0
                int decimales = 6;
                for (double x : {h.minimo, h.por_defecto})
                    if (x > 0) decimales = std::max(decimales, static_cast<int>(std::ceil(-std::log10(x))) + 2);
                s->setDecimals(std::min(decimales, 12));
                s->setRange(h.minimo, h.maximo);
                s->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
                s->setValue(h.por_defecto);
                editor = s;
                break;
            }
            case ENERGIA_TIPO_OPCION: {
                auto* c = new QComboBox(pagina);
                for (const std::string& op : energia::separarOpciones(energia::leerTexto(h.opciones)))
                    c->addItem(QString::fromStdString(op));
                c->setCurrentIndex(static_cast<int>(h.por_defecto));
                editor = c;
                break;
            }
            default: {  // ENERGIA_TIPO_LISTA_ENTEROS
                auto* e = new QLineEdit(QString::fromStdString(energia::leerTexto(h.defecto_texto)), pagina);
                e->setPlaceholderText("ej. 64,32");
                editor = e;
                break;
            }
            }
            const QString ayuda = QString::fromStdString(energia::leerTexto(h.ayuda));
            editor->setToolTip(ayuda);
            form->addRow(QString::fromStdString(energia::leerTexto(h.etiqueta)) + ":", editor);
            if (QWidget* etiqueta = form->labelForField(editor)) etiqueta->setToolTip(ayuda);
            m->editores.append(editor);
        }
        if (m->hiper.empty()) form->addRow(new QLabel("(este modelo no tiene hiperparametros)", pagina));
        return pagina;
    }

    const ModeloDisponible* modeloActual() const
    {
        const int i = cmbModelo_ ? cmbModelo_->currentIndex() : -1;
        return (i >= 0 && i < modelos_.size()) ? &modelos_[i] : nullptr;
    }

    void alCambiarModelo(int indice)
    {
        if (indice < 0 || indice >= modelos_.size()) return;
        pilaParams_->setCurrentIndex(indice);
        const EnergiaInfoModelo& info = modelos_[indice].info;
        lblInfoModelo_->setText(QString("%1 - %2 hilo(s) - DLL: %3")
                                    .arg(QString::fromStdString(energia::leerTexto(info.libreria)))
                                    .arg(info.hilos)
                                    .arg(QFileInfo(modelos_[indice].ruta).fileName()));
    }

    // Lee los campos de la ventana y los valida igual que lo hara la DLL
    bool leerParams(const ModeloDisponible& m, EnergiaParams* p, QJsonObject* json, QString* error)
    {
        valoresPorDefecto(m, p);
        *json = QJsonObject();
        for (size_t i = 0; i < m.hiper.size(); ++i) {
            const EnergiaHiperparametro& h = m.hiper[i];
            EnergiaValor& v = p->valores[i];
            QWidget* w = m.editores.value(static_cast<int>(i));
            const QString clave = QString::fromStdString(energia::leerTexto(h.clave));
            if (auto* s = qobject_cast<QSpinBox*>(w)) {
                v.numero = s->value();
                (*json)[clave] = s->value();
            } else if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) {
                v.numero = d->value();
                (*json)[clave] = d->value();
            } else if (auto* c = qobject_cast<QComboBox*>(w)) {
                v.numero = c->currentIndex();
                (*json)[clave] = c->currentText();
            } else if (auto* e = qobject_cast<QLineEdit*>(w)) {
                energia::copiarTexto(v.texto, sizeof(v.texto), e->text().trimmed().toUtf8().constData());
                (*json)[clave] = QString::fromStdString(energia::leerTexto(v.texto));
            }
        }
        const std::string motivo = energia::validarParams(*p, m.hiper, nullptr);
        if (!motivo.empty()) { *error = "Hiperparametros: " + QString::fromStdString(motivo); return false; }
        return true;
    }

    void actualizarModoDatos(bool sintetico)
    {
        btnCsv_->setEnabled(!sintetico);
        lblCsv_->setEnabled(!sintetico);
        cmbObjetivo_->setEnabled(!sintetico);
    }

    void elegirCsv()
    {
        const QString ruta = QFileDialog::getOpenFileName(this, "Elegir dataset", QString(), "CSV (*.csv);;Todos (*.*)");
        if (ruta.isEmpty()) return;
        QStringList columnas;
        char sep = ',';
        QString error;
        if (!leerEncabezado(ruta, &columnas, &sep, &error)) { log("ERROR: " + error); return; }
        rutaCsv_ = ruta;
        lblCsv_->setText(QFileInfo(ruta).fileName());
        lblCsv_->setToolTip(QDir::toNativeSeparators(ruta));
        cmbObjetivo_->clear();
        cmbObjetivo_->addItems(columnas);
        cmbObjetivo_->setCurrentIndex(columnas.size() - 1);
        log(QString("CSV elegido: %1 (%2 columnas)").arg(QDir::toNativeSeparators(ruta)).arg(columnas.size()));
    }

    void bloquearConfig(bool bloquear)
    {
        for (QWidget* w : panelesConfig_) w->setEnabled(!bloquear);
        btnIniciar_->setEnabled(!bloquear);
        btnCancelar_->setEnabled(bloquear);
    }

    // ---------------------------------------------------------------- inicio
    void iniciarCorrida()
    {
        QString experimento = edExperimento_->text().trimmed();
        experimento.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
        if (experimento.isEmpty()) { log("ERROR: falta el nombre del experimento"); return; }
        if (!chkSintetico_->isChecked() && (rutaCsv_.isEmpty() || cmbObjetivo_->currentIndex() < 0)) {
            log("ERROR: elige un CSV y su columna objetivo, o marca datos sinteticos");
            return;
        }
        const ModeloDisponible* modelo = modeloActual();
        if (!modelo) { log("ERROR: no hay modelo elegido (revisa que haya DLL EnergIA_* y pulsa 'Buscar modelos')"); return; }

        // Hiperparametros validados ANTES de lanzar nada
        EnergiaParams params;
        QJsonObject hiperJson;
        QString error;
        if (!leerParams(*modelo, &params, &hiperJson, &error)) { log("ERROR: " + error); return; }

        FuncionesDll fn;
        if (!cargarDll(*modelo, &fn, &error)) { log("ERROR: " + error); return; }

        // Carpeta: corridas/<experimento>/corrida_NNN
        const QDir base(QCoreApplication::applicationDirPath());
        const QString dirExp = base.filePath(QString(cfg::CARPETA_CORRIDAS) + "/" + experimento);
        numCorrida_ = siguienteNumeroCorrida(dirExp);
        dirCorrida_ = QDir(dirExp).filePath(QString("corrida_%1").arg(numCorrida_, 3, 10, QChar('0')));
        if (!QDir().mkpath(dirCorrida_)) {
            log("ERROR: no se pudo crear " + dirCorrida_);
            descargarDll();
            return;
        }
        experimento_ = experimento;

        // Lo que va a metadata.json sobre el modelo
        const EnergiaInfoModelo& info = modelo->info;
        unidadPaso_ = QString::fromStdString(energia::leerTexto(info.unidad_paso));
        if (unidadPaso_.isEmpty()) unidadPaso_ = "paso";
        modeloJson_ = QJsonObject();
        modeloJson_["clave"] = QString::fromStdString(energia::leerTexto(info.clave));
        modeloJson_["nombre"] = QString::fromStdString(energia::leerTexto(info.nombre));
        modeloJson_["libreria"] = QString::fromStdString(energia::leerTexto(info.libreria));
        modeloJson_["unidad_paso"] = unidadPaso_;
        modeloJson_["hilos"] = info.hilos;
        modeloJson_["dll"] = QFileInfo(modelo->ruta).fileName();
        hiperJson_ = hiperJson;

        // Estado de la corrida
        corriendo_ = true;
        entrenando_ = false;
        deteniendo_ = false;
        cerrando_ = false;
        finalizada_ = false;
        cancelado_ = false;
        estadoFinal_ = "completada";
        errorCorrida_.clear();
        status_ = -1;
        modeloId_ = 0;
        tStartMs_ = tAntesMs_ = tDespuesMs_ = tStopMs_ = 0;
        resumenDatos_ = QJsonObject();
        modeloGuardado_ = QJsonObject();
        evaluacion_ = QJsonObject();
        medidores_.clear();
        barra_->setValue(0);
        bloquearConfig(true);

        // Configuracion que viaja al hilo de trabajo
        ConfigCorrida c;
        c.usarSintetico = chkSintetico_->isChecked();
        c.rutaCsv = rutaCsv_;
        c.columnaObjetivo = cmbObjetivo_->currentIndex();
        c.porcentajeTrain = spPorcentaje_->value();
        c.semilla = static_cast<uint32_t>(spSemilla_->value());
        c.params = params;
        c.params.semilla = c.semilla;  // n_clases lo pone el hilo de trabajo al preparar los datos
        c.fn = fn;
        c.rutaProgreso = QDir(dirCorrida_).filePath("progreso.csv");
        guardarModelo_ = chkGuardar_->isChecked();

        log("----------------------------------------------------------------");
        log(QString("Experimento '%1', corrida %2: %3").arg(experimento_).arg(numCorrida_).arg(QDir::toNativeSeparators(dirCorrida_)));
        fase("Preparando datos");

        Trabajador* t = trabajador_;
        QMetaObject::invokeMethod(t, [t, c] { t->preparar(c); }, Qt::QueuedConnection);
    }

    static int siguienteNumeroCorrida(const QString& dirExp)
    {
        int maximo = 0;
        const QRegularExpression re("^corrida_(\\d+)$");
        for (const QString& d : QDir(dirExp).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const auto m = re.match(d);
            if (m.hasMatch()) maximo = std::max(maximo, m.captured(1).toInt());
        }
        return maximo + 1;
    }

    // Carga solo la DLL del modelo elegido; se descarga al terminar la corrida
    bool cargarDll(const ModeloDisponible& m, FuncionesDll* fn, QString* error)
    {
        if (dll_.isLoaded()) descargarDll();
        dll_.setFileName(m.ruta);
        if (!dll_.load()) { *error = "No se pudo cargar la DLL: " + dll_.errorString(); return false; }

        auto fnVersion = reinterpret_cast<FnVersion>(dll_.resolve("energia_api_version"));
        fn->entrenar = reinterpret_cast<FnEntrenar>(dll_.resolve("entrenar"));
        fn->evaluar = reinterpret_cast<FnEvaluar>(dll_.resolve("evaluar"));
        fn->guardar = reinterpret_cast<FnGuardar>(dll_.resolve("guardar_modelo"));
        fn->liberar = reinterpret_cast<FnLiberar>(dll_.resolve("liberar_modelo"));
        if (!fnVersion || !fn->entrenar || !fn->evaluar || !fn->guardar || !fn->liberar) {
            *error = "La DLL no exporta todas las funciones del contrato (revisar extern \"C\")";
            descargarDll();
            return false;
        }
        const int32_t version = fnVersion();
        if (version != ENERGIA_API_VERSION) {
            *error = QString("Version de contrato distinta: DLL=%1, orquestador=%2").arg(version).arg(ENERGIA_API_VERSION);
            descargarDll();
            return false;
        }
        log("DLL cargada: " + QDir::toNativeSeparators(dll_.fileName()));
        return true;
    }

    void descargarDll()
    {
        if (!dll_.isLoaded()) return;
        if (dll_.unload()) log("DLL descargada");
        else log("ADVERTENCIA: no se pudo descargar la DLL: " + dll_.errorString());
    }

    void alPreparado(bool ok, const QString& error, const QJsonObject& resumen)
    {
        if (!ok) { abortar(error); return; }
        resumenDatos_ = resumen;
        log(QString("Datos listos: %1 filas (%2 entrenamiento / %3 validacion), %4 columnas, %5 clases")
                .arg(resumen["n_filas_total"].toVariant().toLongLong())
                .arg(resumen["n_train"].toVariant().toLongLong())
                .arg(resumen["n_val"].toVariant().toLongLong())
                .arg(resumen["n_cols"].toInt())
                .arg(resumen["n_clases"].toInt()));
        if (cancelado_) { detenerMedidores(); return; }

        // t_start redondeado al siguiente segundo, despues del margen de conexion
        const qint64 t = ahoraMs() + cfg::MARGEN_CONEXION_MS;
        tStartMs_ = ((t + 999) / 1000) * 1000;
        log("t_start = " + QString::fromLatin1(isoUtc(tStartMs_)));
        escribirMetadata("en_curso");

        QString err;
        if (!lanzarMedidores(&err)) { abortar(err); return; }

        fase("Conectando medidores (margen)");
        timerPre_.start(msHasta(tStartMs_));
        timerEntrenar_.start(msHasta(tStartMs_ + cfg::ESTABILIZACION_PRE_MS));
    }

    bool lanzarMedidores(QString* error)
    {
        const QString tStart = QString::fromLatin1(isoUtc(tStartMs_));
        const QString carpeta = QDir::toNativeSeparators(dirCorrida_);
        for (const cfg::Medidor& m : cfg::medidores()) {
            EstadoMedidor e;
            e.nombre = m.nombre;
            e.programa = m.programa;
            e.archivoListo = m.archivoListo;
            e.proceso = new QProcess(this);
            e.proceso->setWorkingDirectory(QCoreApplication::applicationDirPath());
            e.proceso->setProcessChannelMode(QProcess::ForwardedChannels);

            QStringList args;
            for (QString a : m.argumentos) {
                a.replace("{T_START_UTC}", tStart);
                a.replace("{CARPETA_CORRIDA}", carpeta);
                a.replace("{DURACION_MAX_S}", QString::number(cfg::DURACION_MAXIMA_MEDIDORES_S));
                a.replace("{PID_ORQUESTADOR}", QString::number(QCoreApplication::applicationPid()));
                args << a;
            }

            const int indice = medidores_.size();
            connect(e.proceso, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [this, indice](int codigo, QProcess::ExitStatus) { alTerminarMedidor(indice, codigo); });

            // Programa en la carpeta del orquestador tiene prioridad sobre el PATH
            QString programa = m.programa;
            const QFileInfo local(QDir(QCoreApplication::applicationDirPath()).filePath(programa));
            if (QFileInfo(programa).isRelative() && local.exists()) programa = local.absoluteFilePath();
            if (QFileInfo(programa).isAbsolute() && !QFileInfo::exists(programa)) {
                *error = QString("El medidor '%1' no arranco: no existe %2").arg(m.nombre, QDir::toNativeSeparators(programa));
                if (programa.contains("/venv/")) *error += ". Corre medidores/crear_entorno.bat para crear el entorno de Python.";
                return false;
            }

            e.proceso->start(programa, args);
            medidores_.append(e);
            if (!e.proceso->waitForStarted(cfg::ESPERA_ARRANQUE_MEDIDOR_MS)) {
                *error = QString("El medidor '%1' no arranco: %2").arg(m.nombre, e.proceso->errorString());
                return false;
            }
            log(QString("Medidor '%1' lanzado (PID %2)").arg(m.nombre).arg(e.proceso->processId()));
        }
        if (medidores_.isEmpty()) log("Sin medidores configurados: la corrida sigue la linea de tiempo sin medir.");
        return true;
    }

    // Al llegar t_start: los medidores que avisan con archivo deben estar listos
    void alLlegarTStart()
    {
        QStringList faltan;
        for (EstadoMedidor& e : medidores_) {
            if (e.archivoListo.isEmpty()) continue;
            const bool listo = QFile::exists(QDir(dirCorrida_).filePath(e.archivoListo));
            e.listoEnTStart = listo ? 1 : 0;
            if (listo) log(QString("Medidor '%1' listo (%2)").arg(e.nombre, e.archivoListo));
            else faltan << QString("'%1' (falta %2)").arg(e.nombre, e.archivoListo);
        }
        if (!faltan.isEmpty()) {
            abortar("Medidores sin confirmar que estan listos al llegar t_start: " + faltan.join(", ") +
                    ". Si pasa seguido, sube MARGEN_CONEXION_MS.");
            return;
        }
        fase("Estabilizacion pre (linea base)");
    }

    // ---------------------------------------------------------------- entrenamiento
    void lanzarEntrenamiento()
    {
        fase("Entrenando");
        entrenando_ = true;
        Trabajador* t = trabajador_;
        QMetaObject::invokeMethod(t, [t] { t->ejecutar(); }, Qt::QueuedConnection);
    }

    void alProgreso(int faseDll, int stepIndex, int stepTotal, const QString& metricas)
    {
        if (stepTotal <= 0) return;
        if (faseDll == ENERGIA_FASE_START) {
            fase(QString("Entrenando - %1 %2/%3").arg(unidadPaso_).arg(stepIndex + 1).arg(stepTotal));
        } else {
            barra_->setValue(static_cast<int>(100.0 * (stepIndex + 1) / stepTotal));
            log(QString("  %1 %2/%3%4").arg(unidadPaso_).arg(stepIndex + 1).arg(stepTotal).arg(metricas));
        }
    }

    void alTerminar(int status, qint64 tAntesMs, qint64 tDespuesMs, qint64 modeloId)
    {
        entrenando_ = false;
        status_ = status;
        tAntesMs_ = tAntesMs;
        tDespuesMs_ = tDespuesMs;
        modeloId_ = modeloId;
        log(QString("DLL termino: %1, duracion %2 s").arg(describirEstado(status)).arg((tDespuesMs - tAntesMs) / 1000.0, 0, 'f', 3));

        if (status == ENERGIA_OK) {
            fase("Estabilizacion post (linea base)");
            timerPost_.start(static_cast<int>(cfg::ESTABILIZACION_POST_MS));
            return;
        }
        if (status == ENERGIA_CANCELADO) {
            estadoFinal_ = "cancelada";
        } else {
            estadoFinal_ = "error_dll";
            errorCorrida_ = "La DLL devolvio: " + describirEstado(status);
        }
        detenerMedidores();
    }

    // ---------------------------------------------------------------- cancelacion
    void cancelar()
    {
        if (!corriendo_ || deteniendo_ || cancelado_) return;
        cancelado_ = true;
        estadoFinal_ = "cancelada";
        log("Cancelacion solicitada por el usuario");

        if (entrenando_) {
            trabajador_->solicitarCancelacion();  // la DLL se detiene en el proximo aviso del callback
            fase("Cancelando entrenamiento...");
        } else if (tStartMs_ == 0) {
            fase("Cancelando (esperando fin de la preparacion)...");  // alPreparado lo resuelve
        } else {
            detenerMedidores();
        }
    }

    // ---------------------------------------------------------------- medidores
    void alTerminarMedidor(int indice, int codigo)
    {
        if (indice < 0 || indice >= medidores_.size()) return;
        EstadoMedidor& e = medidores_[indice];
        e.codigoSalida = codigo;
        if (!deteniendo_) {
            e.terminoAntes = true;
            log(QString("ADVERTENCIA: el medidor '%1' termino antes de tiempo (codigo %2)").arg(e.nombre).arg(codigo));
        }
        if (deteniendo_ && todosTerminados()) cerrarModelo();
    }

    bool todosTerminados() const
    {
        for (const EstadoMedidor& e : medidores_)
            if (e.proceso && e.proceso->state() != QProcess::NotRunning) return false;
        return true;
    }

    void detenerMedidores()
    {
        if (deteniendo_) return;
        deteniendo_ = true;
        timerPre_.stop();
        timerEntrenar_.stop();
        timerPost_.stop();

        if (medidores_.isEmpty()) { cerrarModelo(); return; }

        fase("Deteniendo medidores");
        tStopMs_ = ahoraMs();
        QFile bandera(QDir(dirCorrida_).filePath("stop.flag"));
        if (bandera.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            bandera.write(isoUtc(tStopMs_));
            bandera.close();
        } else {
            log("ERROR: no se pudo crear stop.flag; se forzara el cierre");
        }

        if (todosTerminados()) { cerrarModelo(); return; }
        timerForzar_.start(static_cast<int>(cfg::ESPERA_CIERRE_MEDIDORES_MS));
    }

    void forzarCierreMedidores()
    {
        for (EstadoMedidor& e : medidores_) {
            if (e.proceso && e.proceso->state() != QProcess::NotRunning) {
                e.forzado = true;
                log(QString("ADVERTENCIA: el medidor '%1' no respondio a stop.flag; se cierra a la fuerza").arg(e.nombre));
                e.proceso->kill();
                e.proceso->waitForFinished(2000);
            }
        }
        cerrarModelo();
    }

    // ---------------------------------------------------------------- cierre
    void cerrarModelo()
    {
        if (cerrando_) return;
        cerrando_ = true;
        timerForzar_.stop();

        const bool evaluar = status_ == ENERGIA_OK && modeloId_ > 0;
        const bool guardar = guardarModelo_ && evaluar;
        const QString ruta = QDir(dirCorrida_).filePath(cfg::ARCHIVO_MODELO);
        if (evaluar) fase(guardar ? "Evaluando y guardando modelo" : "Evaluando modelo");
        modeloGuardado_["solicitado"] = guardarModelo_;
        modeloGuardado_["archivo"] = guardar ? QString(cfg::ARCHIVO_MODELO) : QString();

        Trabajador* t = trabajador_;
        const qint64 id = modeloId_;
        QMetaObject::invokeMethod(t, [t, id, evaluar, guardar, ruta] { t->cerrarModelo(id, evaluar, guardar, ruta); },
                                  Qt::QueuedConnection);
    }

    void alCerrarModelo(int statusGuardar, int statusLiberar, const QJsonObject& evaluacion)
    {
        evaluacion_ = evaluacion;
        if (evaluacion["ejecutada"].toBool()) {
            const int st = evaluacion["status"].toInt();
            QString texto;
            const QJsonObject m = evaluacion["metricas"].toObject();
            for (auto it = m.begin(); it != m.end(); ++it) texto += QString("  %1=%2").arg(it.key()).arg(it.value().toDouble(), 0, 'g', 6);
            if (st == ENERGIA_OK) log(QString("Evaluacion en validacion (%1 ms):%2").arg(evaluacion["duracion_ms"].toVariant().toLongLong()).arg(texto));
            else log("ADVERTENCIA: evaluar modelo: " + describirEstado(st));
        }
        if (statusGuardar >= 0) {
            modeloGuardado_["guardado"] = (statusGuardar == ENERGIA_OK);
            modeloGuardado_["status_guardar"] = statusGuardar;
            log("Guardar modelo: " + describirEstado(statusGuardar));
        } else {
            modeloGuardado_["guardado"] = false;
        }
        if (statusLiberar >= 0 && statusLiberar != ENERGIA_OK) log("ADVERTENCIA: liberar modelo: " + describirEstado(statusLiberar));
        modeloId_ = 0;
        finalizarCorrida();
    }

    void abortar(const QString& motivo)
    {
        log("ABORTADA: " + motivo);
        estadoFinal_ = "abortada";
        errorCorrida_ = motivo;
        detenerMedidores();
    }

    void finalizarCorrida()
    {
        if (finalizada_) return;
        finalizada_ = true;

        escribirMetadata(estadoFinal_);
        descargarDll();
        for (EstadoMedidor& e : medidores_) {
            if (e.proceso) { e.proceso->deleteLater(); e.proceso = nullptr; }
        }
        corriendo_ = false;
        bloquearConfig(false);
        fase("Lista - ultima corrida: " + estadoFinal_);
        log("Corrida finalizada: " + estadoFinal_);
    }

    void escribirMetadata(const QString& estado)
    {
        auto tiempo = [](qint64 ms) { return ms > 0 ? QJsonValue(QString::fromLatin1(isoUtc(ms))) : QJsonValue(QJsonValue::Null); };

        QJsonObject tiempos;
        tiempos["t_start"] = tiempo(tStartMs_);
        tiempos["t_antes"] = tiempo(tAntesMs_);
        tiempos["t_despues"] = tiempo(tDespuesMs_);
        tiempos["t_stop"] = tiempo(tStopMs_);
        tiempos["margen_conexion_ms"] = cfg::MARGEN_CONEXION_MS;
        tiempos["estabilizacion_pre_ms"] = cfg::ESTABILIZACION_PRE_MS;
        tiempos["estabilizacion_post_ms"] = cfg::ESTABILIZACION_POST_MS;

        QJsonArray meds;
        for (const EstadoMedidor& e : medidores_) {
            QJsonObject m;
            m["nombre"] = e.nombre;
            m["programa"] = e.programa;
            m["termino_antes"] = e.terminoAntes;
            m["forzado"] = e.forzado;
            m["codigo_salida"] = e.codigoSalida;
            m["listo_en_t_start"] = (e.listoEnTStart < 0) ? QJsonValue(QJsonValue::Null) : QJsonValue(e.listoEnTStart == 1);
            meds.append(m);
        }

        QJsonObject raiz;
        raiz["experimento"] = experimento_;
        raiz["corrida"] = numCorrida_;
        raiz["estado"] = estado;
        raiz["error"] = errorCorrida_;
        raiz["status_code"] = status_;
        raiz["api_version"] = ENERGIA_API_VERSION;
        raiz["dll"] = QDir::toNativeSeparators(dll_.fileName());
        raiz["modelo"] = modeloJson_;
        raiz["hiperparametros"] = hiperJson_;
        raiz["evaluacion"] = evaluacion_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(evaluacion_);
        raiz["datos"] = resumenDatos_;
        raiz["tiempos"] = tiempos;
        raiz["modelo_guardado"] = modeloGuardado_;
        raiz["medidores"] = meds;

        QFile f(QDir(dirCorrida_).filePath("metadata.json"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(raiz).toJson(QJsonDocument::Indented));
        else log("ERROR: no se pudo escribir metadata.json");
    }

    // ---------------------------------------------------------------- utilidades
    void log(const QString& msg)
    {
        log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + "  " + msg);  // hora local
    }

    void fase(const QString& f) { lblFase_->setText(f); }

    // ---------------------------------------------------------------- estado
    QLineEdit* edExperimento_ = nullptr;
    QCheckBox* chkGuardar_ = nullptr;
    QCheckBox* chkSintetico_ = nullptr;
    QPushButton* btnCsv_ = nullptr;
    QLabel* lblCsv_ = nullptr;
    QComboBox* cmbObjetivo_ = nullptr;
    QSpinBox* spPorcentaje_ = nullptr;
    QSpinBox* spSemilla_ = nullptr;
    QComboBox* cmbModelo_ = nullptr;
    QPushButton* btnBuscar_ = nullptr;
    QLabel* lblInfoModelo_ = nullptr;
    QStackedWidget* pilaParams_ = nullptr;
    QPushButton* btnIniciar_ = nullptr;
    QPushButton* btnCancelar_ = nullptr;
    QLabel* lblFase_ = nullptr;
    QProgressBar* barra_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QList<QWidget*> panelesConfig_;

    QTimer timerPre_, timerEntrenar_, timerPost_, timerForzar_;

    QLibrary dll_;
    QThread hilo_;
    Trabajador* trabajador_ = nullptr;
    QList<EstadoMedidor> medidores_;
    QList<ModeloDisponible> modelos_;

    QString rutaCsv_;
    QString experimento_;
    QString dirCorrida_;
    int numCorrida_ = 0;
    bool guardarModelo_ = false;
    QString unidadPaso_ = "paso";
    QJsonObject modeloJson_;
    QJsonObject hiperJson_;
    QJsonObject resumenDatos_;
    QJsonObject modeloGuardado_;
    QJsonObject evaluacion_;

    QString estadoFinal_;
    QString errorCorrida_;
    qint64 tStartMs_ = 0, tAntesMs_ = 0, tDespuesMs_ = 0, tStopMs_ = 0;
    qint64 modeloId_ = 0;
    int status_ = -1;
    bool corriendo_ = false;
    bool entrenando_ = false;
    bool deteniendo_ = false;
    bool cerrando_ = false;
    bool finalizada_ = false;
    bool cancelado_ = false;
};

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    VentanaPrincipal ventana;
    ventana.show();
    return app.exec();
}

#include "orquestador.moc"