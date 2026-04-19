import asyncio
from contextlib import asynccontextmanager

from fastapi import FastAPI

from src.core.logging import get_logger
from src.services.mqtt_service import mqtt_service
from src.services.telemetria_service import telemetria_service
from src.services.websocket_manager import websocket_manager

logger = get_logger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("=" * 55)
    logger.info("  H-DROP Backend — iniciando")
    logger.info("=" * 55)

    app.state.mqtt = mqtt_service
    app.state.websocket = websocket_manager
    app.state.telemetria = telemetria_service

    mqtt_task = asyncio.create_task(mqtt_service.executar(), name="mqtt-loop")

    try:
        yield
    finally:
        logger.info("H-DROP Backend — encerrando")
        mqtt_task.cancel()
        try:
            await mqtt_task
        except asyncio.CancelledError:
            pass
        await websocket_manager.desconectar_todos()
