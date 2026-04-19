from pydantic import BaseModel, Field


class ComandoDestino(BaseModel):
    """Coordenada de destino enviada ao ESP32 via hdrop/comando."""

    lat: float = Field(ge=-90, le=90)
    lng: float = Field(ge=-180, le=180)


class ComandoResposta(BaseModel):
    status: str
    topico: str
    payload: ComandoDestino
