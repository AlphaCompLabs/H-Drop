import math
from typing import Optional


def calcular_proa(x_bruto: int, y_bruto: int, offset_x: int, offset_y: int) -> float:
    x_calib = x_bruto - offset_x
    y_calib = y_bruto - offset_y
    angulo_graus = math.degrees(math.atan2(y_calib, x_calib))
    if angulo_graus < 0:
        angulo_graus += 360
    return round(angulo_graus, 2)


def parse_nmea_gps(gps_string: str) -> tuple[Optional[float], Optional[float], bool]:
    """
    Extrai (lat, lng, tem_fix) da string NMEA usando os índices validados em campo.
    Índices: partes[5]=lat, partes[6]=hem_lat (S→neg), partes[7]=lng, partes[8]=hem_lng (W→neg).
    """
    if not gps_string or gps_string.startswith(","):
        return None, None, False

    partes = gps_string.split(",")
    if len(partes) <= 8:
        return None, None, False

    try:
        lat_val = float(partes[5])
        lng_val = float(partes[7])
        if partes[6] == "S":
            lat_val *= -1
        if partes[8] == "W":
            lng_val *= -1
        return round(lat_val, 7), round(lng_val, 7), True
    except (ValueError, IndexError):
        return None, None, False
