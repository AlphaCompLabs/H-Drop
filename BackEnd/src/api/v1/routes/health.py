from fastapi import APIRouter, Depends

from src.core.config import settings
from src.services.mqtt_service import MQTTService, mqtt_service
from src.services.websocket_manager import WebSocketManager, websocket_manager

router = APIRouter(tags=["health"])


def get_mqtt() -> MQTTService:
    return mqtt_service


def get_ws() -> WebSocketManager:
    return websocket_manager


@router.get("/health")
async def health() -> dict:
    return {"status": "ok", "app": settings.APP_NAME, "versao": settings.APP_VERSION}


@router.get("/status")
async def status(
    mqtt: MQTTService = Depends(get_mqtt),
    ws: WebSocketManager = Depends(get_ws),
) -> dict:
    return {
        "mqtt": {
            "conectado": mqtt.conectado,
            "broker": settings.MQTT_BROKER,
            "porta": settings.MQTT_PORT,
            "topicos": {
                "raw": settings.TOPICO_RAW,
                "telemetria": settings.TOPICO_TELEMETRIA,
                "comando": settings.TOPICO_COMANDO,
            },
        },
        "websocket": {"clientes": ws.total_clientes},
    }
