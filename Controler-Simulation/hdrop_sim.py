#!/usr/bin/env python3
"""
HDrop ASV — Simulador e Servidor de Dashboard
Simula GNSS, 4G/MQTT, magnetômetro, ultrassônico, bateria e controle PD.
Uso:  python hdrop_sim.py
      Abrir: http://localhost:5000
Requer apenas a stdlib do Python (sem pip install).
"""

import http.server
import json
import math
import os
import random
import socketserver
import threading
import time

# ═══════════════════════════════════════════════════════════════════════════════
#  Simulador do Barco
# ═══════════════════════════════════════════════════════════════════════════════

class BoatSim:
    EARTH_R = 6_371_000.0

    # Parâmetros do ultrassônico (espelha main.c)
    US_MIN, US_MAX  = 20.0, 300.0
    US_ALPHA        = 0.75
    US_N            = 6
    US_SALTO_MAX    = 50.0

    # Parâmetros ESC (espelha main.c)
    ESC_NEUTRO, ESC_MIN, ESC_MAX = 1500, 1000, 2000

    def __init__(self):
        # ── Missão ──────────────────────────────────────────────────────────
        self.lat_a, self.lon_a = -15.8100, -47.8301   # Ponto A (origem)
        self.lat_b, self.lon_b = -15.8109, -47.8288   # Ponto B (~150 m a SE)

        # ── Posição atual ────────────────────────────────────────────────────
        self.lat, self.lon = self.lat_a, self.lon_a

        # ── Navegação ────────────────────────────────────────────────────────
        self.heading  = 0.0    # °  (bússola)
        self.bearing  = 0.0    # °  (rumo ao waypoint)
        self.dist_m   = 0.0    # m  (distância ao waypoint)
        self.sog      = 0.0    # km/h
        self.cog      = 0.0    # °

        # ── ESC ──────────────────────────────────────────────────────────────
        self.pwm_bb = self.ESC_NEUTRO
        self.pwm_eb = self.ESC_NEUTRO

        # ── Bateria (LiFePO4 4S) ─────────────────────────────────────────────
        self.voltage = 12.8
        self.current = 0.5
        self.soc     = 100.0
        self.energy  = 50.0   # Wh totais estimados

        # ── Ultrassônico (pipeline idêntico ao firmware) ─────────────────────
        self._us_janela   = [150.0] * self.US_N
        self._us_idx      = 0
        self._us_cheia    = False
        self._us_valida   = 150.0
        self._us_filtrada = 150.0
        self._us_primeira = True
        self._us_falhas   = 0
        self._us_cnt_al   = 0
        self._us_cnt_liv  = 0
        self._us_alarme   = False
        self._us_fase     = 0.0
        self.us_dist      = 150.0
        self.us_y         = 0.44
        self.us_alarm     = False

        # ── Magnetômetro ─────────────────────────────────────────────────────
        self.mag_x, self.mag_y, self.mag_z = 200, 150, -280

        # ── GNSS ─────────────────────────────────────────────────────────────
        self.gnss_fix = False
        self.hdop     = 3.5
        self.sats     = 2

        # ── FSM ──────────────────────────────────────────────────────────────
        self.fsm      = "CONECTANDO_REDE"
        self.mqtt_ok  = False
        self.mission  = False
        self.arrived  = False

        # ── Histórico de posições (rastro) ───────────────────────────────────
        self.track = []

        # ── Log do sistema ───────────────────────────────────────────────────
        self.logs = []

        # ── Controle interno ─────────────────────────────────────────────────
        self._t    = 0.0
        self._boot = time.time()
        self._lock = threading.Lock()
        self._log("[INIT] Hardware pronto. Criando tarefas FreeRTOS...")
        self._log("[CTR]  task_controle iniciada (20 Hz, Core 1)")
        self._log("[TEL]  task_telemetria iniciada (FSM + 2 Hz, Core 0)")

    # ── Helpers ─────────────────────────────────────────────────────────────

    def _haversine(self, la1, lo1, la2, lo2):
        f1, f2 = math.radians(la1), math.radians(la2)
        df = math.radians(la2 - la1)
        dl = math.radians(lo2 - lo1)
        a  = math.sin(df/2)**2 + math.cos(f1)*math.cos(f2)*math.sin(dl/2)**2
        return self.EARTH_R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))

    def _calc_bearing(self, la1, lo1, la2, lo2):
        f1, f2 = math.radians(la1), math.radians(la2)
        dl = math.radians(lo2 - lo1)
        x  = math.sin(dl) * math.cos(f2)
        y  = math.cos(f1)*math.sin(f2) - math.sin(f1)*math.cos(f2)*math.cos(dl)
        return (math.degrees(math.atan2(x, y)) + 360) % 360

    def _norm180(self, a):
        while a >  180: a -= 360
        while a < -180: a += 360
        return a

    def _log(self, msg):
        ts = time.strftime("%H:%M:%S")
        self.logs.append(f"[{ts}] {msg}")
        if len(self.logs) > 40:
            self.logs.pop(0)

    # ── Loop de atualização (chamado a 20 Hz) ────────────────────────────────

    def update(self, dt):
        with self._lock:
            self._t   += dt
            elapsed    = time.time() - self._boot

            if self.fsm == "CONECTANDO_REDE":
                self._sim_rede(elapsed)
            elif self.fsm == "CONECTANDO_MQTT":
                self._sim_mqtt(elapsed)
            elif self.fsm == "OPERANDO":
                self._update_nav(dt)
                self._update_us(dt)
                self._update_battery(dt)

    def _sim_rede(self, elapsed):
        # Simula aquisição gradual de satélites
        self.sats = min(9, int(elapsed * 3))
        self.hdop = max(1.3, 5.0 - elapsed * 0.8)
        if elapsed > 3.5:
            self.gnss_fix = True
            self.hdop     = 1.3
            self.sats     = 9
            self.fsm      = "CONECTANDO_MQTT"
            self._log("[REDE] Registrado na rede celular (CREG: 0,1)")
            self._log("[GNSS] Módulo GNSS pronto! (+CGNSSPWR: READY!)")
            self._log("[FSM]  Rede e GNSS prontos -> CONECTANDO_MQTT")

    def _sim_mqtt(self, elapsed):
        if elapsed > 7.0:
            self.mqtt_ok = True
            self.mission = True
            self.fsm     = "OPERANDO"
            # Inicializa heading para o bearing inicial
            self.heading = self._calc_bearing(self.lat, self.lon, self.lat_b, self.lon_b)
            self._log("[MQTT] Conectado ao broker HiveMQ!")
            self._log("[MQTT] Subscribe em 'hdrop/comando' confirmado.")
            self._log("[FSM]  MQTT pronto -> OPERANDO")
            self._log(f"[CMD]  Waypoint recebido: lat={self.lat_b}, lon={self.lon_b}")
            self._log("[FSM]  Missão iniciada — navegando para ponto B")

    def _update_nav(self, dt):
        self.bearing = self._calc_bearing(self.lat, self.lon, self.lat_b, self.lon_b)
        self.dist_m  = self._haversine(self.lat, self.lon, self.lat_b, self.lon_b)

        if self.dist_m < 5.0:
            if not self.arrived:
                self.arrived = True
                self.pwm_bb = self.pwm_eb = self.ESC_NEUTRO
                self._log("[MISSÃO] ✓ Destino atingido! ESCs em neutro.")
                self._log("[PUB]   Telemetria final publicada em hdrop/raw")
            self.sog = 0.0
            return

        # ── Controlador PD de heading (espelha Bloco 3 futuro) ──────────────
        err   = self._norm180(self.bearing - self.heading)
        kp    = 2.5
        d_pwm = int(kp * err)
        d_pwm = max(-280, min(280, d_pwm))

        pwm_base   = 1660
        self.pwm_bb = max(self.ESC_MIN, min(self.ESC_MAX, pwm_base + d_pwm))
        self.pwm_eb = max(self.ESC_MIN, min(self.ESC_MAX, pwm_base - d_pwm))

        # Heading converge para bearing (inércia do barco)
        self.heading += self._norm180(self.bearing - self.heading) * min(1.0, dt * 1.2)
        self.heading  = self.heading % 360

        # Velocidade (SOG)
        self.sog = 4.0 + 0.5 * math.sin(self._t * 0.25)

        # Avança posição (sem ruído aqui — posição real é limpa)
        speed_ms  = self.sog / 3.6
        br        = math.radians(self.bearing)
        self.lat += (speed_ms * dt * math.cos(br)) / 111_111
        self.lon += (speed_ms * dt * math.sin(br)) / (111_111 * math.cos(math.radians(self.lat)))

        self.cog  = (self.bearing + random.gauss(0, 1.5)) % 360
        self.hdop = 1.2 + 0.2 * abs(math.sin(self._t * 0.07))

        # Magnetômetro acompanha heading (com ruído de ±8 LSB)
        hr          = math.radians(self.heading)
        self.mag_x  = int(300 * math.cos(hr) + random.gauss(0, 8))
        self.mag_y  = int(300 * math.sin(hr) + random.gauss(0, 8))
        self.mag_z  = -280 + int(random.gauss(0, 4))

        # Rastro de posições
        self.track.append((round(self.lat, 7), round(self.lon, 7)))
        if len(self.track) > 300:
            self.track.pop(0)

        # Log periódico de telemetria (a cada ~5 s de tempo simulado)
        if int(self._t) % 5 == 0 and int(self._t) != getattr(self, '_last_log_t', -1):
            self._last_log_t = int(self._t)
            self._log(f"[GNSS] Fix: lat={self.lat:.6f} lon={self.lon:.6f} SOG={self.sog:.1f}km/h")
            self._log(f"[PUB]  OK -> hdrop/raw | dist={self.dist_m:.0f}m bearing={self.bearing:.1f}°")

    def _update_us(self, dt):
        """Pipeline idêntico ao firmware (us_processar em task_controle, 20 Hz)."""
        # Gerador simulado (onda senoidal + ruído LCG — igual ao firmware)
        self._us_fase = (self._us_fase + 0.0628 * dt * 20) % (2 * math.pi)
        dist_bruta = 150.0 + 100.0 * math.sin(self._us_fase) + random.gauss(0, 5)
        dist_bruta = max(self.US_MIN, min(self.US_MAX, dist_bruta))

        # Rejeição de saltos
        if self._us_primeira:
            self._us_valida  = dist_bruta
            self._us_primeira = False
        else:
            salto = abs(dist_bruta - self._us_valida)
            if salto <= self.US_SALTO_MAX:
                self._us_valida = dist_bruta
            else:
                self._us_valida = 0.7 * self._us_valida + 0.3 * dist_bruta

        # Média móvel circular (janela N=6)
        self._us_janela[self._us_idx] = self._us_valida
        self._us_idx = (self._us_idx + 1) % self.US_N
        if self._us_idx == 0:
            self._us_cheia = True
        n     = self.US_N if self._us_cheia else self._us_idx
        media = sum(self._us_janela[:n]) / n

        # Filtro exponencial (α=0.75)
        self._us_filtrada = self.US_ALPHA * self._us_filtrada + (1 - self.US_ALPHA) * media

        # Normalização [0..1]
        y_norm = max(0.0, min(1.0, (self._us_filtrada - self.US_MIN) / (self.US_MAX - self.US_MIN)))

        # Alarme com histerese (ativa em 2 leituras, desativa em 3)
        threshold = 0.5
        if y_norm < threshold:
            self._us_cnt_al  += 1
            self._us_cnt_liv  = 0
            if self._us_cnt_al >= 2:
                self._us_alarme = True
        else:
            self._us_cnt_liv += 1
            self._us_cnt_al   = 0
            if self._us_cnt_liv >= 3:
                self._us_alarme = False

        self.us_dist  = round(self._us_filtrada, 1)
        self.us_y     = round(y_norm, 3)
        self.us_alarm = self._us_alarme

    def _update_battery(self, dt):
        avg_pwm      = (self.pwm_bb + self.pwm_eb) / 2
        load         = (avg_pwm - 1500) / 500.0
        self.current = max(0.5, 0.5 + 11.0 * load + random.gauss(0, 0.12))
        self.voltage  = 12.8 - (100 - self.soc) * 0.013 + random.gauss(0, 0.01)
        # Drena SOC lentamente (escalonado para visualização no demo)
        self.soc    -= self.current * self.voltage / 3600 * dt / 25
        self.soc     = max(0.0, min(100.0, self.soc))
        self.energy  = round(self.soc * 0.5, 1)

        if int(self.soc) in (80, 60, 40, 20) and abs(self.soc - int(self.soc)) < 0.03:
            self._log(f"[BAT]  SOC={self.soc:.1f}% V={self.voltage:.2f}V I={self.current:.2f}A")

    # ── Snapshot para o frontend ─────────────────────────────────────────────

    def snapshot(self):
        with self._lock:
            return {
                "fsm":     self.fsm,
                "mqtt":    self.mqtt_ok,
                "mission": self.mission,
                "arrived": self.arrived,
                "t":       round(self._t, 1),
                "gnss": {
                    "lat":  round(self.lat + random.gauss(0, 2.7e-6), 7),
                    "lon":  round(self.lon + random.gauss(0, 2.7e-6), 7),
                    "fix":  self.gnss_fix,
                    "sog":  round(self.sog, 2),
                    "cog":  round(self.cog, 1),
                    "hdop": round(self.hdop, 2),
                    "sats": self.sats,
                },
                "nav": {
                    "heading":  round(self.heading, 1),
                    "bearing":  round(self.bearing, 1),
                    "dist_m":   round(self.dist_m, 1),
                    "lat_a": self.lat_a, "lon_a": self.lon_a,
                    "lat_b": self.lat_b, "lon_b": self.lon_b,
                    "track": list(self.track[-60:]),
                },
                "mag": {"x": self.mag_x, "y": self.mag_y, "z": self.mag_z},
                "us":  {
                    "dist_cm": self.us_dist,
                    "y_norm":  self.us_y,
                    "alarm":   self.us_alarm,
                },
                "bat": {
                    "v":   round(self.voltage, 2),
                    "i":   round(self.current, 2),
                    "soc": round(self.soc, 1),
                    "wh":  self.energy,
                },
                "esc": {"bb": self.pwm_bb, "eb": self.pwm_eb},
                "logs": list(self.logs[-14:]),
            }


# ═══════════════════════════════════════════════════════════════════════════════
#  Servidor HTTP
# ═══════════════════════════════════════════════════════════════════════════════

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SIM      = BoatSim()


class Handler(http.server.BaseHTTPRequestHandler):

    def do_GET(self):
        if self.path in ("/", "/dashboard.html"):
            self._file(os.path.join(BASE_DIR, "dashboard.html"), "text/html; charset=utf-8")
        elif self.path == "/api/state":
            body = json.dumps(SIM.snapshot()).encode()
            self._send(200, "application/json", body)
        else:
            self.send_error(404)

    def _file(self, path, ctype):
        try:
            with open(path, "rb") as f:
                data = f.read()
            self._send(200, ctype, data)
        except FileNotFoundError:
            self.send_error(404, f"Não encontrado: {path}")

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", len(body))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass   # silencia o log padrão


# ═══════════════════════════════════════════════════════════════════════════════
#  Ponto de entrada
# ═══════════════════════════════════════════════════════════════════════════════

def _sim_loop():
    dt = 1.0 / 20.0
    while True:
        SIM.update(dt)
        time.sleep(dt)


if __name__ == "__main__":
    t = threading.Thread(target=_sim_loop, daemon=True)
    t.start()

    PORT = 5000
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", PORT), Handler) as srv:
        print()
        print("  ╔══════════════════════════════════════════╗")
        print("  ║  HDrop ASV — Simulador de Dashboard      ║")
        print("  ╚══════════════════════════════════════════╝")
        print(f"\n  → Abra no navegador: http://localhost:{PORT}")
        print("  → Ctrl+C para encerrar\n")
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\n  Simulador encerrado.")
