// =============================================================================
// system_meter.cpp  —  Medidor de sistema de EnergIA (v2)
// -----------------------------------------------------------------------------
// Proceso independiente con un solo hilo. Mide CPU y RAM del sistema, del
// proceso orquestador (que incluye la DLL de entrenamiento y sus hilos) y del
// propio medidor, sobre la rejilla temporal que arranca en t_start.
//
// Salidas en la carpeta de la corrida:
//   sistema_metrics.csv  una fila por intervalo, flush despues de cada fila
//   sistema_info.json    datos fijos del equipo + estado y cierre del medidor
//
// Ciclo de vida:
//   1. Arranca durante el margen de conexion, prepara todo y escribe el
//      encabezado del CSV. No mide nada antes de t_start.
//   2. En t_start toma una lectura de referencia (no se escribe).
//   3. Cada INTERVALO_MS escribe una fila con lo ocurrido en el intervalo.
//      timestamp          = punto teorico de la rejilla (t_start + n * intervalo),
//                           igual convencion que atorch_ble_capture.py, para que
//                           energia y sistema se crucen por igualdad de timestamp.
//      timestamp_lectura  = hora real en que se tomo la lectura.
//      intervalo_ms       = duracion real entre esta lectura y la anterior.
//      La fila de las HH:MM:17.000Z resume lo ocurrido entre la lectura anterior
//      (~16.000) y la actual (~17.000).
//   4. Termina al aparecer stop.flag o al llegar a t_start + duracion maxima.
//      Si el orquestador muere, sus columnas quedan vacias y se sigue midiendo
//      el sistema hasta stop.flag o el tope.
//
// Uso (el orquestador reemplaza los marcadores):
//   system_meter.exe --t-start T_START_UTC --carpeta CARPETA_CORRIDA
//                    --duration DURACION_MAX_S --pid PID_ORQUESTADOR
//   Pruebas manuales:
//     --t-start ahora   arranca en el siguiente segundo entero + 3 s
//     --pid 0 u omitido no mide el proceso orquestador
//
// Compilar (g++ del kit MinGW de Qt):
//   g++ -O2 -std=c++17 -static -o system_meter.exe system_meter.cpp -lpdh -lwinmm -lshell32
// =============================================================================

#define __USE_MINGW_ANSI_STDIO 1
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

// ============================ CONSTANTES =====================================
constexpr int  INTERVALO_MS          = 1000;  // periodo de muestreo (igual que el Atorch)
constexpr int  PASO_ESPERA_MS        = 200;   // cada cuanto se revisa stop.flag al esperar
constexpr bool SUBIR_PRIORIDAD       = true;  // ABOVE_NORMAL para no atrasar muestras con CPU saturada
constexpr bool MEDIR_POR_NUCLEO      = true;  // columnas nucleo_NN_ocupado_s
constexpr bool MEDIR_RENDIMIENTO_CPU = true;  // % Processor Performance (turbo / frecuencia real)
constexpr int  SEGUNDOS_TSTART_AHORA = 3;     // solo para --t-start ahora

constexpr const wchar_t* NOMBRE_CSV  = L"sistema_metrics.csv";
constexpr const wchar_t* NOMBRE_INFO = L"sistema_info.json";
constexpr const wchar_t* NOMBRE_STOP = L"stop.flag";
constexpr int VERSION_MEDIDOR = 2;

// ============================ TIEMPO =========================================
// Tiempo = FILETIME en uint64: unidades de 100 ns desde 1601-01-01 UTC.
using Tiempo = uint64_t;
constexpr Tiempo TICKS_MS = 10000ULL;
constexpr Tiempo TICKS_S  = 10000000ULL;

static uint64_t ft64(const FILETIME& f) {
    return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

static Tiempo ahora() {
    FILETIME f;
    GetSystemTimePreciseAsFileTime(&f);  // reloj de pared UTC, resolucion < 1 us
    return ft64(f);
}

static std::string iso_utc(Tiempo t) {
    FILETIME f;
    f.dwLowDateTime  = DWORD(t & 0xFFFFFFFFULL);
    f.dwHighDateTime = DWORD(t >> 32);
    SYSTEMTIME s;
    FileTimeToSystemTime(&f, &s);
    char b[40];
    snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
             unsigned(s.wYear), unsigned(s.wMonth), unsigned(s.wDay),
             unsigned(s.wHour), unsigned(s.wMinute), unsigned(s.wSecond),
             unsigned(s.wMilliseconds));
    return b;
}

// Acepta "2026-09-22T00:52:45Z", "...45.3Z", "...45.334Z" (la Z es opcional).
static bool parse_iso_utc(const std::wstring& txt, Tiempo& out) {
    unsigned y, mo, d, h, mi, se;
    int pos = 0;
    if (swscanf(txt.c_str(), L"%4u-%2u-%2uT%2u:%2u:%2u%n",
                &y, &mo, &d, &h, &mi, &se, &pos) != 6)
        return false;
    size_t i = size_t(pos);
    unsigned ms = 0;
    if (i < txt.size() && txt[i] == L'.') {
        ++i;
        unsigned escala = 100;
        while (i < txt.size() && iswdigit(txt[i])) {
            ms += unsigned(txt[i] - L'0') * escala;
            escala /= 10;
            ++i;
        }
    }
    if (i < txt.size() && (txt[i] == L'Z' || txt[i] == L'z')) ++i;
    if (i != txt.size()) return false;

    SYSTEMTIME s{};
    s.wYear = WORD(y); s.wMonth = WORD(mo); s.wDay = WORD(d);
    s.wHour = WORD(h); s.wMinute = WORD(mi); s.wSecond = WORD(se);
    s.wMilliseconds = WORD(ms);
    FILETIME f;
    if (!SystemTimeToFileTime(&s, &f)) return false;
    out = ft64(f);
    return true;
}

// ============================ UTILIDADES =====================================
static bool existe(const std::wstring& ruta) {
    return GetFileAttributesW(ruta.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool es_directorio(const std::wstring& ruta) {
    DWORD a = GetFileAttributesW(ruta.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string a_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string json_str(const std::string& s) {
    if (s.empty()) return "null";
    std::string r = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
                else r += char(c);
        }
    }
    return r + "\"";
}

// Espera hasta 'objetivo' revisando stop.flag cada PASO_ESPERA_MS.
// Devuelve false si aparecio stop.flag.
static bool esperar_hasta(Tiempo objetivo, const std::wstring& ruta_stop) {
    for (;;) {
        if (existe(ruta_stop)) return false;
        Tiempo t = ahora();
        if (t >= objetivo) return true;
        uint64_t resta_ms = (objetivo - t + TICKS_MS - 1) / TICKS_MS;
        Sleep(DWORD(std::min<uint64_t>(resta_ms, PASO_ESPERA_MS)));
    }
}

// ============================ FUENTES DE DATOS ===============================
// CPU por nucleo: NtQuerySystemInformation(SystemProcessorPerformanceInformation)
// da tiempos acumulados crudos por nucleo, con la misma semantica que
// GetSystemTimes (el tiempo kernel incluye el idle). Cubre el grupo de
// procesadores actual (hasta 64 nucleos logicos).
struct SPPI {
    LARGE_INTEGER IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG InterruptCount;
};
using FnNtQSI = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION = 8;

static FnNtQSI           g_ntqsi   = nullptr;
static std::vector<SPPI> g_buf_nuc;
static size_t            g_nucleos = 0;   // nucleos con columna en el CSV

static bool leer_nucleos(std::vector<uint64_t>& out) {
    ULONG len = 0;
    LONG st = g_ntqsi(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, g_buf_nuc.data(),
                      ULONG(g_buf_nuc.size() * sizeof(SPPI)), &len);
    if (st != 0) return false;
    size_t n = len / sizeof(SPPI);
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        uint64_t k  = uint64_t(g_buf_nuc[i].KernelTime.QuadPart);
        uint64_t id = uint64_t(g_buf_nuc[i].IdleTime.QuadPart);
        uint64_t u  = uint64_t(g_buf_nuc[i].UserTime.QuadPart);
        out[i] = (k >= id ? k - id : 0) + u;   // tiempo ocupado acumulado
    }
    return true;
}

// PDH: rendimiento del procesador (puede pasar de 100 con turbo) y frecuencia base.
static PDH_HQUERY   g_pdh        = nullptr;
static PDH_HCOUNTER g_cont_rend  = nullptr;
static PDH_HCOUNTER g_cont_freq  = nullptr;

static bool pdh_valor(PDH_HCOUNTER c, double& v) {
    if (!c) return false;
    PDH_FMT_COUNTERVALUE val;
    if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &val) != ERROR_SUCCESS)
        return false;
    if (val.CStatus != PDH_CSTATUS_VALID_DATA && val.CStatus != PDH_CSTATUS_NEW_DATA)
        return false;
    v = val.doubleValue;
    return true;
}

// Procesos: tiempo de CPU (kernel + user de todos sus hilos) y memoria.
static HANDLE g_orq = nullptr;

static bool leer_proceso(HANDLE h, uint64_t& cpu, uint64_t& ws, uint64_t& priv) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return false;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
        return false;
    cpu  = ft64(k) + ft64(u);
    ws   = pmc.WorkingSetSize;
    priv = pmc.PrivateUsage;
    return true;
}

// ============================ LECTURA ========================================
struct Lectura {
    Tiempo   t = 0;
    bool     sis_ok = false;
    uint64_t sis_idle = 0, sis_kernel = 0, sis_user = 0;
    uint64_t ram_usada = 0;
    std::vector<uint64_t> nucleo;          // ocupado acumulado por nucleo
    bool     orq_ok = false;
    uint64_t orq_cpu = 0, orq_ws = 0, orq_priv = 0;
    bool     med_ok = false;
    uint64_t med_cpu = 0, med_ws = 0, med_priv = 0;
    bool     rend_ok = false;
    double   rendimiento = 0.0;
};

static Lectura leer() {
    Lectura L;
    L.t = ahora();

    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu)) {
        L.sis_ok = true;
        L.sis_idle = ft64(fi); L.sis_kernel = ft64(fk); L.sis_user = ft64(fu);
    }
    if (g_ntqsi) leer_nucleos(L.nucleo);

    if (g_orq && WaitForSingleObject(g_orq, 0) == WAIT_TIMEOUT)
        L.orq_ok = leer_proceso(g_orq, L.orq_cpu, L.orq_ws, L.orq_priv);
    L.med_ok = leer_proceso(GetCurrentProcess(), L.med_cpu, L.med_ws, L.med_priv);

    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) L.ram_usada = m.ullTotalPhys - m.ullAvailPhys;

    if (g_pdh && PdhCollectQueryData(g_pdh) == ERROR_SUCCESS)
        L.rend_ok = pdh_valor(g_cont_rend, L.rendimiento);

    return L;
}

// ============================ CSV ============================================
static uint64_t delta(uint64_t a, uint64_t b) { return b >= a ? b - a : 0; }

static void col_seg(std::string& l, uint64_t ticks) {
    char b[40]; snprintf(b, sizeof b, ",%.4f", double(ticks) / double(TICKS_S)); l += b;
}
static void col_u64(std::string& l, uint64_t v) {
    char b[40]; snprintf(b, sizeof b, ",%llu", (unsigned long long)v); l += b;
}
static void col_dbl(std::string& l, double v, int dec) {
    char b[48]; snprintf(b, sizeof b, ",%.*f", dec, v); l += b;
}
static void col_vacia(std::string& l, int n = 1) { l.append(size_t(n), ','); }

static std::string encabezado_csv() {
    std::string h = "timestamp,timestamp_lectura,intervalo_ms,"
                    "cpu_sis_ocupado_s,cpu_sis_total_s,ram_sis_usada_bytes,rendimiento_cpu_pct,"
                    "orq_cpu_s,orq_ws_bytes,orq_privado_bytes,"
                    "med_cpu_s,med_ws_bytes,med_privado_bytes";
    for (size_t i = 0; i < g_nucleos; ++i) {
        char b[40]; snprintf(b, sizeof b, ",nucleo_%02u_ocupado_s", unsigned(i)); h += b;
    }
    return h + "\n";
}

static std::string fila_csv(Tiempo t_rejilla, const Lectura& a, const Lectura& b) {
    std::string l = iso_utc(t_rejilla);
    l += ',';
    l += iso_utc(b.t);
    col_dbl(l, double(b.t - a.t) / double(TICKS_MS), 3);

    if (a.sis_ok && b.sis_ok) {
        uint64_t dk = delta(a.sis_kernel, b.sis_kernel);  // incluye idle
        uint64_t du = delta(a.sis_user,   b.sis_user);
        uint64_t di = delta(a.sis_idle,   b.sis_idle);
        uint64_t total = dk + du;
        col_seg(l, total >= di ? total - di : 0);
        col_seg(l, total);
    } else {
        col_vacia(l, 2);
    }
    col_u64(l, b.ram_usada);
    if (b.rend_ok) col_dbl(l, b.rendimiento, 2); else col_vacia(l);

    if (a.orq_ok && b.orq_ok) {
        col_seg(l, delta(a.orq_cpu, b.orq_cpu));
        col_u64(l, b.orq_ws);
        col_u64(l, b.orq_priv);
    } else {
        col_vacia(l, 3);
    }
    if (a.med_ok && b.med_ok) {
        col_seg(l, delta(a.med_cpu, b.med_cpu));
        col_u64(l, b.med_ws);
        col_u64(l, b.med_priv);
    } else {
        col_vacia(l, 3);
    }
    for (size_t i = 0; i < g_nucleos; ++i) {
        if (i < a.nucleo.size() && i < b.nucleo.size()) col_seg(l, delta(a.nucleo[i], b.nucleo[i]));
        else col_vacia(l);
    }
    return l + "\n";
}

// ============================ INFO JSON ======================================
struct Info {
    std::string  estado = "iniciando";   // iniciando | esperando | midiendo | terminado | error
    std::string  motivo_fin;             // stop_flag | stop_flag_antes_de_t_start | duracion_maxima | error
    std::string  error;
    std::string  t_start, t_referencia, t_fin;
    std::wstring carpeta;
    long long    duracion_max_s = 0;
    unsigned long pid_orq = 0, pid_med = 0;
    std::string  orq_estado = "no_solicitado";  // no_solicitado | no_encontrado | vivo | terminado
    std::string  orq_fin_detectado;
    unsigned     nucleos_logicos = 0;
    size_t       nucleos_medidos = 0;
    uint64_t     ram_total = 0;
    bool         freq_ok = false;
    double       freq_base_mhz = 0.0;
    bool         rend_disponible = false;
    bool         prioridad_alta = false;
    bool         temporizador_1ms = false;
    bool         inicio_tardio = false;
    uint64_t     filas = 0;
    uint64_t     muestras_saltadas = 0;
};

static bool escribir_info(const std::wstring& ruta, const Info& I) {
    char num[64];
    std::string j = "{\n";
    auto campo = [&](const char* k, const std::string& v, bool ultimo = false) {
        j += "  \""; j += k; j += "\": "; j += v; j += ultimo ? "\n" : ",\n";
    };
    auto n_u = [&](unsigned long long v) { snprintf(num, sizeof num, "%llu", v); return std::string(num); };
    auto n_b = [](bool v) { return std::string(v ? "true" : "false"); };

    campo("medidor", "\"system_meter\"");
    campo("version", n_u(VERSION_MEDIDOR));
    campo("estado", json_str(I.estado));
    campo("motivo_fin", json_str(I.motivo_fin));
    campo("error", json_str(I.error));
    campo("carpeta", json_str(a_utf8(I.carpeta)));
    campo("t_start", json_str(I.t_start));
    campo("t_referencia", json_str(I.t_referencia));
    campo("t_fin", json_str(I.t_fin));
    campo("inicio_tardio", n_b(I.inicio_tardio));
    campo("intervalo_ms", n_u(INTERVALO_MS));
    campo("duracion_max_s", n_u((unsigned long long)I.duracion_max_s));
    campo("pid_orquestador", n_u(I.pid_orq));
    campo("orquestador_estado", json_str(I.orq_estado));
    campo("orquestador_fin_detectado", json_str(I.orq_fin_detectado));
    campo("pid_medidor", n_u(I.pid_med));
    campo("prioridad_alta", n_b(I.prioridad_alta));
    campo("temporizador_1ms", n_b(I.temporizador_1ms));
    campo("convencion_timestamp", "\"rejilla_fin_de_intervalo\"");
    campo("nucleos_logicos", n_u(I.nucleos_logicos));
    campo("nucleos_medidos", n_u(I.nucleos_medidos));
    campo("ram_total_bytes", n_u(I.ram_total));
    if (I.freq_ok) { snprintf(num, sizeof num, "%.0f", I.freq_base_mhz); campo("frecuencia_base_mhz", num); }
    else campo("frecuencia_base_mhz", "null");
    campo("rendimiento_cpu_disponible", n_b(I.rend_disponible));
    campo("filas_escritas", n_u(I.filas));
    campo("muestras_saltadas", n_u(I.muestras_saltadas), true);
    j += "}\n";

    // Escritura atomica: archivo temporal + reemplazo.
    std::wstring tmp = ruta + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
    ok = (fclose(f) == 0) && ok;
    return ok && MoveFileExW(tmp.c_str(), ruta.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ============================ MAIN ===========================================
static int uso() {
    fprintf(stderr,
        "Uso: system_meter.exe --t-start <ISO UTC|ahora> --carpeta <ruta> "
        "--duration <segundos> [--pid <pid>]\n");
    return 2;
}

int main() {
    // ---- Argumentos (en UTF-16 para soportar rutas con tildes) ----
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return uso();

    std::wstring arg_tstart, carpeta;
    long long duracion_s = -1;
    unsigned long pid = 0;
    bool args_ok = true;
    for (int i = 1; i < argc && args_ok; ++i) {
        std::wstring a = argv[i];
        bool hay_valor = (i + 1 < argc);
        if      (a == L"--t-start"  && hay_valor) arg_tstart = argv[++i];
        else if (a == L"--carpeta"  && hay_valor) carpeta    = argv[++i];
        else if (a == L"--duration" && hay_valor) duracion_s = wcstoll(argv[++i], nullptr, 10);
        else if (a == L"--pid"      && hay_valor) pid        = wcstoul(argv[++i], nullptr, 10);
        else {
            fprintf(stderr, "Argumento no reconocido o sin valor: %s\n", a_utf8(a).c_str());
            args_ok = false;
        }
    }
    LocalFree(argv);
    if (!args_ok || carpeta.empty() || arg_tstart.empty() || duracion_s <= 0) return uso();

    while (carpeta.size() > 3 && (carpeta.back() == L'\\' || carpeta.back() == L'/'))
        carpeta.pop_back();

    Tiempo t_start = 0;
    if (arg_tstart == L"ahora") {
        t_start = (ahora() / TICKS_S + 1 + SEGUNDOS_TSTART_AHORA) * TICKS_S;
    } else if (!parse_iso_utc(arg_tstart, t_start)) {
        fprintf(stderr, "t_start invalido: %s\n", a_utf8(arg_tstart).c_str());
        return uso();
    }

    CreateDirectoryW(carpeta.c_str(), nullptr);  // la crea el orquestador; esto es para pruebas
    if (!es_directorio(carpeta)) {
        fprintf(stderr, "No existe la carpeta de la corrida: %s\n", a_utf8(carpeta).c_str());
        return 3;
    }
    const std::wstring ruta_csv  = carpeta + L"\\" + NOMBRE_CSV;
    const std::wstring ruta_info = carpeta + L"\\" + NOMBRE_INFO;
    const std::wstring ruta_stop = carpeta + L"\\" + NOMBRE_STOP;

    Info info;
    info.carpeta        = carpeta;
    info.t_start        = iso_utc(t_start);
    info.duracion_max_s = duracion_s;
    info.pid_orq        = pid;
    info.pid_med        = GetCurrentProcessId();

    // ---- Preparacion (durante el margen de conexion) ----
    if (SUBIR_PRIORIDAD)
        info.prioridad_alta = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != 0;
    // Sleep con resolucion de ~1 ms. Se registra en sistema_info.json porque
    // es una (pequena) perturbacion del sistema medido que conviene declarar.
    info.temporizador_1ms = (timeBeginPeriod(1) == TIMERR_NOERROR);

    if (pid != 0) {
        g_orq = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
        if (!g_orq) info.orq_estado = "no_encontrado";
        else if (WaitForSingleObject(g_orq, 0) == WAIT_OBJECT_0) info.orq_estado = "terminado";
        else info.orq_estado = "vivo";
    }

    info.nucleos_logicos = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (MEDIR_POR_NUCLEO) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt)
            g_ntqsi = reinterpret_cast<FnNtQSI>(
                reinterpret_cast<void*>(GetProcAddress(nt, "NtQuerySystemInformation")));
        if (g_ntqsi) {
            g_buf_nuc.resize(std::max<size_t>(info.nucleos_logicos, 1));
            std::vector<uint64_t> prueba;
            if (leer_nucleos(prueba) && !prueba.empty()) g_nucleos = prueba.size();
            else g_ntqsi = nullptr;
        }
    }
    info.nucleos_medidos = g_nucleos;

    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) info.ram_total = m.ullTotalPhys;

    if (MEDIR_RENDIMIENTO_CPU && PdhOpenQueryW(nullptr, 0, &g_pdh) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(g_pdh, L"\\Processor Information(_Total)\\% Processor Performance",
                                  0, &g_cont_rend) != ERROR_SUCCESS)
            g_cont_rend = nullptr;
        if (PdhAddEnglishCounterW(g_pdh, L"\\Processor Information(_Total)\\Processor Frequency",
                                  0, &g_cont_freq) != ERROR_SUCCESS)
            g_cont_freq = nullptr;
        if (!g_cont_rend && !g_cont_freq) {
            PdhCloseQuery(g_pdh);
            g_pdh = nullptr;
        } else if (PdhCollectQueryData(g_pdh) == ERROR_SUCCESS) {
            info.freq_ok = pdh_valor(g_cont_freq, info.freq_base_mhz);
        }
    }
    info.rend_disponible = (g_cont_rend != nullptr);

    FILE* csv = _wfopen(ruta_csv.c_str(), L"w");
    if (!csv) {
        info.estado = "error"; info.motivo_fin = "error";
        info.error = "no se pudo crear sistema_metrics.csv";
        escribir_info(ruta_info, info);
        fprintf(stderr, "No se pudo crear el CSV\n");
        if (info.temporizador_1ms) timeEndPeriod(1);
        return 3;
    }
    fputs(encabezado_csv().c_str(), csv);
    fflush(csv);

    info.estado = "esperando";
    escribir_info(ruta_info, info);

    auto terminar = [&](const char* motivo) {
        info.estado = "terminado";
        info.motivo_fin = motivo;
        info.t_fin = iso_utc(ahora());
        escribir_info(ruta_info, info);
        fclose(csv);
        if (g_pdh) PdhCloseQuery(g_pdh);
        if (g_orq) CloseHandle(g_orq);
        if (info.temporizador_1ms) timeEndPeriod(1);
        return 0;
    };

    // ---- Punto de referencia sobre la rejilla ----
    const Tiempo intervalo = Tiempo(INTERVALO_MS) * TICKS_MS;
    const Tiempo t_limite  = t_start + Tiempo(duracion_s) * TICKS_S;
    Tiempo t_ref = t_start;
    Tiempo t_ya  = ahora();
    if (t_ya > t_start) {  // arranco tarde: siguiente punto de la rejilla
        info.inicio_tardio = true;
        t_ref = t_start + ((t_ya - t_start + intervalo - 1) / intervalo) * intervalo;
    }
    info.t_referencia = iso_utc(t_ref);

    if (!esperar_hasta(t_ref, ruta_stop)) return terminar("stop_flag_antes_de_t_start");

    Lectura previa = leer();  // referencia: no se escribe
    info.estado = "midiendo";
    escribir_info(ruta_info, info);

    // ---- Bucle de muestreo ----
    Tiempo objetivo = t_ref + intervalo;
    for (;;) {
        if (objetivo > t_limite) return terminar("duracion_maxima");
        if (!esperar_hasta(objetivo, ruta_stop)) return terminar("stop_flag");

        Lectura actual = leer();
        const std::string fila = fila_csv(objetivo, previa, actual);
        fputs(fila.c_str(), csv);
        fflush(csv);
        ++info.filas;

        if (g_orq && info.orq_estado == "vivo" && WaitForSingleObject(g_orq, 0) == WAIT_OBJECT_0) {
            info.orq_estado = "terminado";
            info.orq_fin_detectado = iso_utc(actual.t);
            escribir_info(ruta_info, info);
            fprintf(stderr, "El proceso orquestador termino; se sigue midiendo el sistema\n");
        }

        previa = actual;
        objetivo += intervalo;
        // Si el equipo se atraso (suspension, bloqueo largo), saltar al siguiente
        // punto futuro de la rejilla en vez de disparar filas seguidas.
        Tiempo t = ahora();
        if (t >= objetivo) {
            uint64_t saltos = (t - objetivo) / intervalo + 1;
            objetivo += saltos * intervalo;
            info.muestras_saltadas += saltos;
        }
    }
}
