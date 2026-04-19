from enum import Enum
from typing import Optional

from pydantic import BaseModel, ConfigDict, Field


class StatusGPS(str, Enum):
    FIXADO = "FIXADO"
    SEM_SINAL = "SEM_SINAL"


class RawTelemetria(BaseModel):
    """Payload cru publicado pelo ESP32 em hdrop/raw."""

    model_config = ConfigDict(extra="allow")

    g: str = Field(default="", description="String NMEA do GPS")
    m: list[int] = Field(default_factory=lambda: [0, 0, 0], description="Vetor [x, y, z] do magnetômetro")


class GPSProcessado(BaseModel):
    status: StatusGPS
    lat: Optional[float] = None
    lng: Optional[float] = None


class TelemetriaProcessada(BaseModel):
    """Payload publicado em hdrop/telemetria e no WebSocket /ws/telemetria."""

    proa: float = Field(ge=0, lt=360, description="Proa magnética em graus")
    gps: GPSProcessado
