#!/usr/bin/env python3
# =============================================================================
# energia_ble.py  —  Medidor de energía BLE de EnergIA (v1.3)
# -----------------------------------------------------------------------------
# Proceso independiente con un solo hilo (asyncio). Captura el Atorch S1BW por
# BLE y escribe una fila por segundo sobre la rejilla t_start + n·1 s.
# Base: scripts/atorch_ble_capture.py de Juan Diego
#       (repo JuanDPenaM/proactive-autoscaling-dl-models), reescrito para EnergIA.
#
# Modos:
#   medir     Lo lanza el orquestador. Escribe en la carpeta de la corrida:
#               energia_atorch.csv, energia_info.json, energia_log.txt,
#               energia_lista.flag (con la primera trama válida).
#             Termina con stop.flag, con el tope de duración, con Ctrl+C o
#             cuando termina el orquestador (--pid), para no quedar huérfano
#             reteniendo la única conexión BLE que acepta el Atorch.
#   calibrar  Se corre a mano. Registra la llegada de cada trama en
#             intervalos_tramas.csv y al final sugiere el umbral de trama vieja.
#
# Tres tareas en el mismo hilo:
#   vigilante  cada 250 ms revisa stop.flag, el tope, Ctrl+C y si el orquestador
#              sigue vivo; si toca, cancela
#              las demás (la cancelación interrumpe búsquedas y conexiones).
#   conexión   busca la MAC, conecta, se suscribe, detecta caídas y reconecta
#              sin límite de intentos. Solo actualiza el estado en memoria.
#   rejilla    en cada t_start + n·1 s escribe una fila con el estado actual,
#              esté conectado o no (solo en modo medir).
#
# Códigos de salida:
#   0 terminó por stop.flag (o Ctrl+C) y recibió datos
#   1 error inesperado
#   2 terminó por el tope de duración y recibió datos
#   3 nunca recibió una trama válida (tiene prioridad sobre 0 y 2)
#   4 argumentos o configuración inválidos
#   5 el orquestador terminó antes de crear stop.flag (el medidor se cerró solo)
# =============================================================================

from __future__ import annotations

import argparse
import asyncio
import csv
import json
import math
import os
import platform
import signal
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Optional

VERSION_MEDIDOR = "1.3"

# =============================================================================
# 1. CONSTANTES DEL LABORATORIO
# =============================================================================

MAC_ATORCH = "00:00:00:19:EC:2C"                          # Atorch S1BW
CHAR_NOTIFY = "0000ffe1-0000-1000-8000-00805f9b34fb"      # característica con notify

# Mapa de campos de la trama (validado contra la pantalla del S1BW).
# nombre: (offset, bytes, escala, decimales). Big-endian, sin signo.
CAMPOS = {
    "voltage_v":  (4,  3, 0.1,   1),
    "current_a":  (7,  3, 0.001, 3),
    "power_w":    (10, 3, 0.1,   1),
    "energy_kwh": (13, 4, 0.01,  2),
}
CABECERA_TRAMA = b"\xff\x55"          # las tramas observadas empiezan con ff5501010004
LARGO_MINIMO_TRAMA = 17               # hasta el final de energy_kwh

INTERVALO_REJILLA_S = 1.0             # una fila por segundo, igual que system_meter
UMBRAL_TRAMA_VIEJA_S = 3.0            # calibrado 2026-09-25: 20 min, 1194 intervalos, p99.9 1.234 s, max 1.286 s
GAP_SIN_TRAMAS_S = 8.0                # sin tramas este tiempo -> se da por caída y se reconecta
BUSQUEDA_TIMEOUT_S = 10.0             # búsqueda de la MAC por intento
CONEXION_TIMEOUT_S = 15.0             # conexión por intento
PAUSA_REINTENTO_S = 2.0               # entre intentos fallidos o tras una caída
REPETICION_FALLO_S = 30.0             # un mismo fallo repetido se vuelve a anotar cada 30 s
REVISION_SESION_S = 0.5               # cada cuánto se revisa si la conexión sigue viva
VIGILANTE_S = 0.25                    # cada cuánto se revisa stop.flag / tope / Ctrl+C
CIERRE_DESCONEXION_S = 3.0            # límite para stop_notify + disconnect al cerrar
ATRASO_AVISO_S = 0.5                  # si una fila sale con más atraso, se anota en el log
MAX_EVENTOS_INFO = 500                # eventos de conexión guardados en el JSON

DURACION_CALIBRACION_S = 1200         # 20 min: ~1000 intervalos, suficiente para el p99,9

# Nombres de archivo dentro de la carpeta de la corrida
ARCH_CSV = "energia_atorch.csv"
ARCH_INFO = "energia_info.json"
ARCH_LOG = "energia_log.txt"
ARCH_LISTA = "energia_lista.flag"
ARCH_STOP = "stop.flag"
ARCH_INTERVALOS = "intervalos_tramas.csv"

CTRL_C = False  # lo activa el manejador de Ctrl+C; lo lee el vigilante

COLUMNAS_CSV = ["timestamp", "timestamp_trama", "conectado", *CAMPOS.keys(), "raw_hex"]

# =============================================================================
# 2. UTILIDADES
# =============================================================================

def ahora_utc() -> datetime:
    return datetime.now(timezone.utc)


def iso_utc(dt: datetime) -> str:
    """ISO UTC con milisegundos: 2026-09-22T00:52:45.334Z (formato común de EnergIA)."""
    dt = dt.astimezone(timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond // 1000:03d}Z"


def leer_iso_utc(texto: str) -> datetime:
    s = texto.strip()
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    dt = datetime.fromisoformat(s)
    if dt.tzinfo is None:
        raise ValueError("el instante debe indicar zona horaria (terminar en Z)")
    return dt.astimezone(timezone.utc)


def decodificar(trama: bytes) -> Optional[dict]:
    """Devuelve los campos decodificados, o None si la trama no es válida."""
    if len(trama) < LARGO_MINIMO_TRAMA or not trama.startswith(CABECERA_TRAMA):
        return None
    valores = {}
    for nombre, (offset, largo, escala, _dec) in CAMPOS.items():
        crudo = int.from_bytes(trama[offset:offset + largo], byteorder="big", signed=False)
        valores[nombre] = crudo * escala
    return valores


def formatear(nombre: str, valor: float) -> str:
    return f"{valor:.{CAMPOS[nombre][3]}f}"


def version_bleak() -> str:
    try:
        from importlib.metadata import version
        return version("bleak")
    except Exception:
        return "desconocida"


def escribir_json_atomico(ruta: Path, datos: dict) -> None:
    tmp = ruta.with_suffix(ruta.suffix + ".tmp")
    tmp.write_text(json.dumps(datos, indent=2, ensure_ascii=False), encoding="utf-8")
    os.replace(tmp, ruta)


class Registro:
    """Escribe cada mensaje en consola y en energia_log.txt, con hora UTC."""

    def __init__(self, ruta: Optional[Path]):
        self._f = ruta.open("a", encoding="utf-8") if ruta else None

    def __call__(self, mensaje: str) -> None:
        linea = f"[{iso_utc(ahora_utc())}] {mensaje}"
        print(linea, flush=True)
        if self._f:
            self._f.write(linea + "\n")
            self._f.flush()

    def cerrar(self) -> None:
        if self._f:
            self._f.close()
            self._f = None


def temporizador_1ms(activar: bool) -> bool:
    """Sube la resolución del temporizador de Windows a 1 ms, como system_meter."""
    if sys.platform != "win32":
        return False
    try:
        import ctypes
        winmm = ctypes.WinDLL("winmm")
        r = winmm.timeBeginPeriod(1) if activar else winmm.timeEndPeriod(1)
        return r == 0
    except Exception:
        return False

class VigiaProceso:
    """Vigila si el proceso del orquestador sigue vivo.

    En Windows abre un handle al proceso al arrancar: el handle sigue apuntando a
    ESE proceso aunque Windows reutilice su PID después, así que no hay falsos
    "vivo". Si el PID no se puede abrir, se anota y no se vigila (el tope de
    duración sigue siendo la red de seguridad).
    """

    def __init__(self, pid: Optional[int]):
        self.pid = pid
        self.estado = "no_vigilado" if pid is None else "no_encontrado"
        self._handle = None
        if pid is None:
            return
        if sys.platform == "win32":
            try:
                import ctypes
                from ctypes import wintypes
                k32 = ctypes.WinDLL("kernel32", use_last_error=True)
                k32.OpenProcess.restype = wintypes.HANDLE
                k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
                k32.WaitForSingleObject.restype = wintypes.DWORD
                k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
                k32.CloseHandle.argtypes = [wintypes.HANDLE]
                SYNCHRONIZE = 0x00100000
                h = k32.OpenProcess(SYNCHRONIZE, False, pid)
                if h:
                    self._k32, self._handle = k32, h
                    self.estado = "vivo"
            except Exception:
                pass
        else:
            try:
                os.kill(pid, 0)
                self.estado = "vivo"
            except OSError:
                pass

    def sigue_vivo(self) -> bool:
        """True si no se vigila o si el proceso sigue corriendo."""
        if self.estado != "vivo":
            return True
        if sys.platform == "win32":
            WAIT_TIMEOUT = 0x00000102
            vivo = self._k32.WaitForSingleObject(self._handle, 0) == WAIT_TIMEOUT
        else:
            try:
                os.kill(self.pid, 0)
                vivo = True
            except OSError:
                vivo = False
        if not vivo:
            self.estado = "terminado"
        return vivo

    def cerrar(self) -> None:
        if self._handle:
            self._k32.CloseHandle(self._handle)
            self._handle = None

# =============================================================================
# 3. ESTADO COMPARTIDO (lo escribe la conexión, lo lee la rejilla)
# =============================================================================

@dataclass
class Trama:
    mono: float          # time.monotonic() de llegada, para calcular edad
    ts_iso: str          # hora UTC de llegada
    valores: dict
    raw_hex: str


class Estado:
    def __init__(self, log: Registro, carpeta: Path):
        self.log = log
        self.carpeta = carpeta
        self.conectado = False
        self.sesion_activa: Optional[int] = None
        self.sesiones = 0                     # conexiones exitosas
        self.caidas = 0
        self.intentos_fallidos = 0
        self.ultima: Optional[Trama] = None
        self.ult_notif_mono = 0.0             # última notificación de la sesión (válida o no)
        self.tramas_validas = 0
        self.tramas_invalidas = 0
        self.primera_trama_iso: Optional[str] = None
        self.filas_escritas = 0
        self.filas_con_datos = 0
        self.error: Optional[str] = None
        self.ultimo_fallo: Optional[str] = None
        self.ultimo_fallo_mono = 0.0
        self.fallos_repetidos = 0
        self.filas_atrasadas = 0
        self.atraso_max_s = 0.0
        self.eventos: list[dict] = []
        self.eventos_descartados = 0
        # modo calibrar
        self.cal_escritor = None
        self.cal_archivo = None
        self.cal_prev_mono: Optional[float] = None
        self.cal_intervalos: list[float] = []

    def evento(self, tipo: str, detalle: str = "") -> None:
        if len(self.eventos) < MAX_EVENTOS_INFO:
            self.eventos.append({"hora": iso_utc(ahora_utc()), "tipo": tipo, "detalle": detalle})
        else:
            self.eventos_descartados += 1

# =============================================================================
# 4. TAREA DE CONEXIÓN
# =============================================================================

def al_recibir(est: Estado, sesion: int, datos: bytearray, modo: str) -> None:
    """Callback de notify: solo actualiza el estado en memoria."""
    if est.sesion_activa != sesion:
        return  # notificación rezagada de una sesión anterior
    mono = time.monotonic()
    llegada = ahora_utc()
    est.ult_notif_mono = mono
    trama = bytes(datos)
    valores = decodificar(trama)
    if valores is None:
        est.tramas_invalidas += 1
        return

    est.tramas_validas += 1
    est.ultima = Trama(mono, iso_utc(llegada), valores, trama.hex())

    if est.primera_trama_iso is None:
        est.primera_trama_iso = est.ultima.ts_iso
        est.log(f"Primera trama válida: {formatear('power_w', valores['power_w'])} W")
        if modo == "medir":
            (est.carpeta / ARCH_LISTA).write_text(est.primera_trama_iso + "\n", encoding="utf-8")
            est.log(f"Aviso de listo escrito: {ARCH_LISTA}")

    if modo == "calibrar" and est.cal_escritor is not None:
        intervalo = ""
        if est.cal_prev_mono is not None:
            dt = mono - est.cal_prev_mono
            est.cal_intervalos.append(dt)
            intervalo = f"{dt:.3f}"
        est.cal_prev_mono = mono
        est.cal_escritor.writerow([est.tramas_validas, sesion, est.ultima.ts_iso, intervalo,
                                   len(trama), formatear("power_w", valores["power_w"])])
        est.cal_archivo.flush()


def anotar_fallo(est: Estado, mensaje: str) -> None:
    """Anota un fallo; si es el mismo que el anterior, solo cada REPETICION_FALLO_S."""
    ahora = time.monotonic()
    if mensaje != est.ultimo_fallo or ahora - est.ultimo_fallo_mono >= REPETICION_FALLO_S:
        extra = f" (repetido {est.fallos_repetidos} veces)" if est.fallos_repetidos else ""
        est.log(f"{mensaje}{extra}; reintento cada {PAUSA_REINTENTO_S:g} s")
        est.evento("fallo", mensaje[:200])
        est.ultimo_fallo, est.ultimo_fallo_mono, est.fallos_repetidos = mensaje, ahora, 0
    else:
        est.fallos_repetidos += 1


async def cerrar_cliente(cliente) -> None:
    async def _cerrar():
        if cliente.is_connected:
            try:
                await cliente.stop_notify(CHAR_NOTIFY)
            except Exception:
                pass
            await cliente.disconnect()
    try:
        await asyncio.wait_for(_cerrar(), CIERRE_DESCONEXION_S)
    except Exception:
        pass  # si WinRT se cuelga se abandona; Windows libera la conexión al salir


async def vigilar_sesion(est: Estado, cliente, desconectado: asyncio.Event, t_conexion: float) -> str:
    """Espera hasta que la sesión se caiga y devuelve el motivo."""
    while True:
        await asyncio.sleep(REVISION_SESION_S)
        if desconectado.is_set() or not cliente.is_connected:
            return "desconexion_reportada"
        # ult_notif_mono puede ser de la sesión anterior; se toma desde la conexión
        base = max(est.ult_notif_mono, t_conexion)
        if time.monotonic() - base > GAP_SIN_TRAMAS_S:
            return f"sin_tramas_{GAP_SIN_TRAMAS_S:g}s"


async def tarea_conexion(est: Estado, modo: str) -> None:
    from bleak import BleakClient, BleakScanner

    est.log(f"Buscando {MAC_ATORCH} (búsqueda de hasta {BUSQUEDA_TIMEOUT_S:g} s por intento)...")
    while True:
        est.conectado = False
        try:
            dispositivo = await BleakScanner.find_device_by_address(
                MAC_ATORCH, timeout=BUSQUEDA_TIMEOUT_S)
            fallo = None if dispositivo is not None else "no visible"
        except asyncio.CancelledError:
            raise
        except Exception as e:
            # p. ej. Bluetooth de Windows apagado: la búsqueda falla de inmediato
            dispositivo, fallo = None, f"{type(e).__name__}: {e}"
        if dispositivo is None:
            est.intentos_fallidos += 1
            anotar_fallo(est, f"Búsqueda de {MAC_ATORCH} fallida ({fallo})")
            await asyncio.sleep(PAUSA_REINTENTO_S)
            continue
        est.log(f"{MAC_ATORCH} visible")
        est.ultimo_fallo = None

        sesion = est.sesiones + 1
        desconectado = asyncio.Event()

        def al_desconectar(_cliente, sesion=sesion, ev=desconectado):
            if est.sesion_activa == sesion:
                est.conectado = False   # la rejilla vacía los valores desde ya
                ev.set()

        cliente = None
        try:
            cliente = BleakClient(dispositivo, disconnected_callback=al_desconectar,
                                  timeout=CONEXION_TIMEOUT_S)
            est.log("Conectando...")
            await cliente.connect()
            car = cliente.services.get_characteristic(CHAR_NOTIFY)
            if car is None or "notify" not in car.properties:
                raise RuntimeError(f"la característica {CHAR_NOTIFY} no existe o no tiene notify")

            est.sesiones = sesion
            est.sesion_activa = sesion
            est.cal_prev_mono = None      # los intervalos no cruzan sesiones
            await cliente.start_notify(
                car, lambda _c, datos, s=sesion: al_recibir(est, s, datos, modo))
            t_conexion = time.monotonic()
            est.conectado = True
            est.evento("conectado", f"sesion {sesion}")
            est.log(f"Conectado (sesión {sesion}), suscrito a {CHAR_NOTIFY}")

            motivo = await vigilar_sesion(est, cliente, desconectado, t_conexion)
            est.caidas += 1
            est.evento("caida", motivo)
            est.log(f"Caída de la sesión {sesion}: {motivo}. Reconectando...")
        except asyncio.CancelledError:
            raise
        except Exception as e:
            est.intentos_fallidos += 1
            anotar_fallo(est, f"Fallo de conexión ({type(e).__name__}: {e})")
        finally:
            est.conectado = False
            est.sesion_activa = None
            if cliente is not None:
                await cerrar_cliente(cliente)

        await asyncio.sleep(PAUSA_REINTENTO_S)

# =============================================================================
# 5. TAREA DE REJILLA (modo medir)
# =============================================================================

def avisar_atraso(est: Estado, filas: int, atraso_max: float) -> None:
    est.filas_atrasadas += filas
    est.atraso_max_s = max(est.atraso_max_s, atraso_max)
    est.log(f"Aviso: {filas} fila(s) escritas con atraso (máximo {atraso_max:.2f} s)")


async def tarea_rejilla(est: Estado, t_start: datetime, escritor, archivo) -> None:
    espera = (t_start - ahora_utc()).total_seconds()
    if espera > 0:
        est.log(f"Esperando t_start ({espera:.1f} s)")
    elif -espera > ATRASO_AVISO_S:
        est.log(f"Aviso: t_start ya pasó hace {-espera:.1f} s; esas filas se escriben de inmediato y vacías")
    n = 0
    pendientes, atraso_max = 0, 0.0
    while True:
        objetivo = t_start + timedelta(seconds=n * INTERVALO_REJILLA_S)
        espera = (objetivo - ahora_utc()).total_seconds()
        if espera > 0:
            if pendientes:
                avisar_atraso(est, pendientes, atraso_max)
                pendientes, atraso_max = 0, 0.0
            await asyncio.sleep(espera)
        elif -espera > ATRASO_AVISO_S:
            pendientes += 1
            atraso_max = max(atraso_max, -espera)

        tr = est.ultima
        conectado = est.conectado
        fresca = (tr is not None and conectado
                  and (time.monotonic() - tr.mono) <= UMBRAL_TRAMA_VIEJA_S)
        fila = [iso_utc(objetivo), tr.ts_iso if tr else "", 1 if conectado else 0]
        if fresca:
            fila += [formatear(k, tr.valores[k]) for k in CAMPOS] + [tr.raw_hex]
            est.filas_con_datos += 1
        else:
            fila += [""] * len(CAMPOS) + [""]
        escritor.writerow(fila)
        archivo.flush()
        est.filas_escritas += 1
        n += 1

# =============================================================================
# 6. TAREA VIGILANTE
# =============================================================================

async def tarea_vigilante(est: Estado, t_fin: datetime, vigia: Optional[VigiaProceso] = None) -> str:
    stop = est.carpeta / ARCH_STOP
    while True:
        if CTRL_C:
            return "ctrl_c"
        if stop.exists():
            return "stop_flag"
        if vigia is not None and not vigia.sigue_vivo():
            return "orquestador_terminado"
        if ahora_utc() >= t_fin:
            return "tope"
        await asyncio.sleep(VIGILANTE_S)

# =============================================================================
# 7. EJECUCIÓN Y CIERRE ORDENADO
# =============================================================================

async def ejecutar(est: Estado, t_fin: datetime, trabajos: list,
                   vigia: Optional[VigiaProceso] = None) -> str:
    vigilante = asyncio.create_task(tarea_vigilante(est, t_fin, vigia))
    otras = [asyncio.create_task(t) for t in trabajos]
    hechas, _ = await asyncio.wait([vigilante, *otras], return_when=asyncio.FIRST_COMPLETED)

    if vigilante in hechas:
        motivo = vigilante.result()
    else:
        motivo = "error"
        for t in hechas:
            exc = t.exception()
            est.error = f"{type(exc).__name__}: {exc}" if exc else "una tarea terminó sin motivo"
        vigilante.cancel()

    est.log(f"Cerrando por: {motivo}" + (f" ({est.error})" if est.error else ""))
    for t in otras:
        t.cancel()
    await asyncio.wait(otras, timeout=CIERRE_DESCONEXION_S + 1.0)
    return motivo


def codigo_salida(est: Estado, motivo: str) -> int:
    if motivo == "error":
        return 1
    if est.tramas_validas == 0:
        return 3
    if motivo == "orquestador_terminado":
        return 5
    if motivo == "tope":
        return 2
    return 0


def info_base(modo: str, carpeta: Path, timer_ok: bool) -> dict:
    return {
        "medidor": "energia_ble.py",
        "version": VERSION_MEDIDOR,
        "modo": modo,
        "carpeta": str(carpeta.resolve()),
        "pid": os.getpid(),
        "argumentos": sys.argv[1:],
        "dispositivo": {"modelo": "Atorch S1BW", "mac": MAC_ATORCH, "caracteristica": CHAR_NOTIFY},
        "campos": {k: {"offset": o, "bytes": b, "escala": e, "byteorder": "big", "signed": False}
                   for k, (o, b, e, _d) in CAMPOS.items()},
        "parametros": {
            "intervalo_rejilla_s": INTERVALO_REJILLA_S,
            "umbral_trama_vieja_s": UMBRAL_TRAMA_VIEJA_S,
            "gap_sin_tramas_s": GAP_SIN_TRAMAS_S,
            "busqueda_timeout_s": BUSQUEDA_TIMEOUT_S,
            "conexion_timeout_s": CONEXION_TIMEOUT_S,
            "pausa_reintento_s": PAUSA_REINTENTO_S,
            "vigilante_s": VIGILANTE_S,
            "cierre_desconexion_s": CIERRE_DESCONEXION_S,
        },
        "convencion_timestamp": "timestamp = punto teórico de la rejilla (t_start + n·1 s); "
                                "timestamp_trama = hora real de llegada de la trama usada",
        "regla_valores_vacios": "vacíos si no hay trama, si conectado = 0 o si la trama "
                                f"tiene más de {UMBRAL_TRAMA_VIEJA_S:g} s",
        "entorno": {
            "python": platform.python_version(),
            "bleak": version_bleak(),
            "sistema": platform.platform(),
            "temporizador_1ms": timer_ok,
        },
    }


def resumen_cierre(est: Estado, motivo: str, codigo: int) -> dict:
    return {
        "motivo": motivo,
        "codigo_salida": codigo,
        "hora_cierre": iso_utc(ahora_utc()),
        "error": est.error,
        "primera_trama": est.primera_trama_iso,
        "tramas_validas": est.tramas_validas,
        "tramas_invalidas": est.tramas_invalidas,
        "filas_escritas": est.filas_escritas,
        "filas_con_datos": est.filas_con_datos,
        "filas_atrasadas": est.filas_atrasadas,
        "atraso_max_s": round(est.atraso_max_s, 3),
        "sesiones": est.sesiones,
        "caidas": est.caidas,
        "intentos_fallidos": est.intentos_fallidos,
        "eventos": est.eventos,
        "eventos_descartados": est.eventos_descartados,
    }

# =============================================================================
# 8. MODOS
# =============================================================================

def percentil(ordenados: list[float], p: float) -> float:
    """Percentil por rango más cercano."""
    k = max(1, math.ceil(p / 100.0 * len(ordenados)))
    return ordenados[k - 1]


def modo_medir(args, log: Registro, carpeta: Path) -> int:
    t_start = leer_iso_utc(args.t_start_utc)
    t_fin = t_start + timedelta(seconds=args.duracion_max_s)
    est = Estado(log, carpeta)
    timer_ok = temporizador_1ms(True)
    vigia = VigiaProceso(args.pid)
    if args.pid is not None and vigia.estado != "vivo":
        log(f"Aviso: no se encontró el orquestador (PID {args.pid}); no se vigilará")

    info = info_base("medir", carpeta, timer_ok)
    info["t_start"] = iso_utc(t_start)
    info["t_fin_tope"] = iso_utc(t_fin)
    info["duracion_max_s"] = args.duracion_max_s
    info["pid_orquestador"] = args.pid
    info["orquestador_estado_inicio"] = vigia.estado
    escribir_json_atomico(carpeta / ARCH_INFO, info)

    log(f"Medidor BLE v{VERSION_MEDIDOR} | t_start {iso_utc(t_start)} | tope {iso_utc(t_fin)}")
    log(f"Python {info['entorno']['python']} | bleak {info['entorno']['bleak']}")

    motivo = "error"
    with (carpeta / ARCH_CSV).open("w", newline="", encoding="utf-8") as f:
        escritor = csv.writer(f)
        escritor.writerow(COLUMNAS_CSV)
        f.flush()
        try:
            motivo = asyncio.run(ejecutar(est, t_fin, [
                tarea_conexion(est, "medir"),
                tarea_rejilla(est, t_start, escritor, f),
            ], vigia))
        except Exception as e:
            est.error = f"{type(e).__name__}: {e}"
            log(f"Error inesperado: {est.error}")
            motivo = "error"

    temporizador_1ms(False)
    vigia.cerrar()
    codigo = codigo_salida(est, motivo)
    info["cierre"] = resumen_cierre(est, motivo, codigo)
    info["cierre"]["orquestador_estado_fin"] = vigia.estado
    escribir_json_atomico(carpeta / ARCH_INFO, info)
    log(f"Fin: {est.filas_escritas} filas ({est.filas_con_datos} con datos), "
        f"{est.tramas_validas} tramas, {est.caidas} caídas. Código {codigo}")
    return codigo


def modo_calibrar(args, log: Registro, carpeta: Path) -> int:
    t_fin = ahora_utc() + timedelta(seconds=args.duracion_s)
    est = Estado(log, carpeta)

    info = info_base("calibrar", carpeta, False)
    info["duracion_s"] = args.duracion_s
    escribir_json_atomico(carpeta / ARCH_INFO, info)
    log(f"Calibración de {args.duracion_s} s. Detener antes: Ctrl+C o crear {ARCH_STOP} en la carpeta.")

    motivo = "error"
    with (carpeta / ARCH_INTERVALOS).open("w", newline="", encoding="utf-8") as f:
        est.cal_archivo = f
        est.cal_escritor = csv.writer(f)
        est.cal_escritor.writerow(["n", "sesion", "timestamp_llegada", "intervalo_s",
                                   "largo_trama", "power_w"])
        f.flush()
        try:
            motivo = asyncio.run(ejecutar(est, t_fin, [tarea_conexion(est, "calibrar")]))
        except Exception as e:
            est.error = f"{type(e).__name__}: {e}"
            log(f"Error inesperado: {est.error}")
            motivo = "error"

    codigo = codigo_salida(est, motivo)
    if codigo == 2:
        codigo = 0  # en calibración, llegar al final de la duración es lo normal
    info["cierre"] = resumen_cierre(est, motivo, codigo)

    iv = sorted(est.cal_intervalos)
    if iv:
        p999 = percentil(iv, 99.9)
        resumen = {
            "intervalos": len(iv),
            "promedio_s": round(sum(iv) / len(iv), 3),
            "minimo_s": round(iv[0], 3),
            "p50_s": round(percentil(iv, 50), 3),
            "p99_s": round(percentil(iv, 99), 3),
            "p99_9_s": round(p999, 3),
            "maximo_s": round(iv[-1], 3),
            "umbral_sugerido_s": math.ceil(p999 + 1.0),
            "nota": "intervalos solo dentro de una misma sesión; los huecos por caídas no cuentan",
        }
        info["calibracion"] = resumen
        log("Resumen de intervalos entre tramas:")
        for k, v in resumen.items():
            if k != "nota":
                log(f"  {k}: {v}")
        if len(iv) < 1000:
            log("Aviso: menos de 1000 intervalos; el p99,9 es poco confiable. Conviene una captura más larga.")
    else:
        log("Sin intervalos suficientes para calcular el umbral.")

    escribir_json_atomico(carpeta / ARCH_INFO, info)
    log(f"Fin de calibración. Código {codigo}")
    return codigo

# =============================================================================
# 9. PUNTO DE ENTRADA
# =============================================================================

class ParserEnergia(argparse.ArgumentParser):
    """argparse sale con 2 ante un error; aquí el 2 significa 'tope', así que se usa 4."""
    def error(self, message):
        self.print_usage(sys.stderr)
        print(f"energia_ble.py: error: {message}", file=sys.stderr, flush=True)
        sys.exit(4)


def leer_argumentos():
    p = ParserEnergia(description="Medidor de energía BLE de EnergIA (Atorch S1BW)")
    sub = p.add_subparsers(dest="modo", required=True, parser_class=ParserEnergia)

    m = sub.add_parser("medir", help="medición de una corrida (lo lanza el orquestador)")
    m.add_argument("--t-start-utc", required=True, help="instante común, p. ej. 2026-09-24T15:30:00.000Z")
    m.add_argument("--carpeta", required=True, help="carpeta de la corrida (debe existir)")
    m.add_argument("--duracion-max-s", required=True, type=int, help="tope contado desde t_start")
    m.add_argument("--pid", type=int, default=None,
                   help="PID del orquestador; si termina, el medidor se cierra solo (opcional)")

    c = sub.add_parser("calibrar", help="mide los intervalos entre tramas para fijar el umbral")
    c.add_argument("--duracion-s", type=int, default=DURACION_CALIBRACION_S)
    c.add_argument("--carpeta", default="", help="por defecto calibracion_<fecha UTC>")
    return p.parse_args()


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    args = leer_argumentos()

    # Validación de configuración (código 4)
    try:
        import bleak  # noqa: F401
    except ImportError:
        print("Falta el paquete 'bleak'. Corre crear_entorno.bat.", file=sys.stderr, flush=True)
        return 4

    if args.modo == "medir":
        carpeta = Path(args.carpeta)
        if not carpeta.is_dir():
            print(f"La carpeta de la corrida no existe: {carpeta}", file=sys.stderr, flush=True)
            return 4
        try:
            leer_iso_utc(args.t_start_utc)
        except ValueError as e:
            print(f"--t-start-utc inválido ({args.t_start_utc}): {e}", file=sys.stderr, flush=True)
            return 4
        if args.duracion_max_s <= 0:
            print("--duracion-max-s debe ser mayor que 0", file=sys.stderr, flush=True)
            return 4
        if args.pid is not None and args.pid <= 0:
            print("--pid debe ser mayor que 0", file=sys.stderr, flush=True)
            return 4
    else:
        if args.duracion_s <= 0:
            print("--duracion-s debe ser mayor que 0", file=sys.stderr, flush=True)
            return 4
        nombre = args.carpeta or f"calibracion_{ahora_utc().strftime('%Y%m%d_%H%M%S')}"
        carpeta = Path(nombre)
        carpeta.mkdir(parents=True, exist_ok=True)

    log = Registro(carpeta / ARCH_LOG)

    # Ctrl+C: solo marca; el vigilante hace el cierre ordenado
    def al_ctrl_c(_signum, _frame):
        global CTRL_C
        CTRL_C = True
    signal.signal(signal.SIGINT, al_ctrl_c)

    try:
        if args.modo == "medir":
            return modo_medir(args, log, carpeta)
        return modo_calibrar(args, log, carpeta)
    except Exception as e:
        log(f"Error inesperado fuera del ciclo: {type(e).__name__}: {e}")
        return 1
    finally:
        log.cerrar()


if __name__ == "__main__":
    sys.exit(main())