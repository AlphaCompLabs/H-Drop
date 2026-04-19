from fastapi import APIRouter, Depends, HTTPException, status

from src.core.config import settings
from src.schemas.comando import ComandoDestino, ComandoResposta
from src.services.mqtt_service import MQTTService, mqtt_service
from src.services.telemetria_service import TelemetriaService, telemetria_service

router = APIRouter(prefix="/comando", tags=["comando"])


def get_mqtt() -> MQTTService:
    return mqtt_service


def get_telemetria() -> TelemetriaService:
    return telemetria_service


@router.post("", response_model=ComandoResposta, status_code=status.HTTP_200_OK)
async def enviar_destino(
    destino: ComandoDestino,
    mqtt: MQTTService = Depends(get_mqtt),
    tele: TelemetriaService = Depends(get_telemetria),
) -> ComandoResposta:
    ok = await mqtt.publicar(settings.TOPICO_COMANDO, destino.model_dump(), qos=1)
    if not ok:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Broker MQTT indisponível",
        )
    tele.registrar_destino(destino)
    return ComandoResposta(status="publicado", topico=settings.TOPICO_COMANDO, payload=destino)


@router.get("/ultimo", response_model=ComandoDestino | None)
async def ultimo_destino(tele: TelemetriaService = Depends(get_telemetria)) -> ComandoDestino | None:
    return tele.ultimo_destino
