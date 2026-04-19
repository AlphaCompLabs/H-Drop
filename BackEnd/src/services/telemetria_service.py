from typing import Optional

from src.core.config import settings
from src.core.logging import get_logger
from src.schemas.comando import ComandoDestino
from src.schemas.telemetria import GPSProcessado, RawTelemetria, StatusGPS, TelemetriaProcessada
from src.utils.geo import haversine_metros
from src.utils.sensores import calcular_proa, parse_nmea_gps

logger = get_logger(__name__)


class TelemetriaService:
    """Processa telemetria bruta, guarda o último estado e último destino."""

    def __init__(self) -> None:
        self._ultima_telemetria: Optional[TelemetriaProcessada] = None
        self._ultimo_destino: Optional[ComandoDestino] = None

    def processar_raw(self, raw: RawTelemetria) -> TelemetriaProcessada:
        lat, lng, tem_fix = parse_nmea_gps(raw.g)

        mag = raw.m if len(raw.m) >= 2 else [0, 0, 0]
        proa = calcular_proa(mag[0], mag[1], settings.MAG_OFFSET_X, settings.MAG_OFFSET_Y)

        gps = GPSProcessado(
            status=StatusGPS.FIXADO if tem_fix else StatusGPS.SEM_SINAL,
            lat=lat,
            lng=lng,
        )
        telemetria = TelemetriaProcessada(proa=proa, gps=gps)
        self._ultima_telemetria = telemetria

        if tem_fix and self._ultimo_destino and lat is not None and lng is not None:
            dist = haversine_metros(lat, lng, self._ultimo_destino.lat, self._ultimo_destino.lng)
            logger.info("GPS FIX | proa=%.1f° lat=%s lng=%s | dist ao destino: %.1f m", proa, lat, lng, dist)
        elif tem_fix:
            logger.info("GPS FIX | proa=%.1f° lat=%s lng=%s", proa, lat, lng)
        else:
            logger.info("GPS SEM SINAL | proa=%.1f°", proa)

        return telemetria

    def registrar_destino(self, destino: ComandoDestino) -> None:
        self._ultimo_destino = destino
        logger.info("Destino registrado: lat=%.7f lng=%.7f", destino.lat, destino.lng)

    @property
    def ultima_telemetria(self) -> Optional[TelemetriaProcessada]:
        return self._ultima_telemetria

    @property
    def ultimo_destino(self) -> Optional[ComandoDestino]:
        return self._ultimo_destino


telemetria_service = TelemetriaService()
