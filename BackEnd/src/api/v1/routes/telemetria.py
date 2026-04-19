from fastapi import APIRouter, Depends, WebSocket, WebSocketDisconnect

from src.schemas.telemetria import TelemetriaProcessada
from src.services.telemetria_service import TelemetriaService, telemetria_service
from src.services.websocket_manager import WebSocketManager, websocket_manager

router = APIRouter(tags=["telemetria"])


def get_telemetria() -> TelemetriaService:
    return telemetria_service


def get_ws() -> WebSocketManager:
    return websocket_manager


@router.get("/telemetria/ultima", response_model=TelemetriaProcessada | None)
async def ultima_telemetria(tele: TelemetriaService = Depends(get_telemetria)) -> TelemetriaProcessada | None:
    return tele.ultima_telemetria


@router.websocket("/ws/telemetria")
async def ws_telemetria(
    ws: WebSocket,
    manager: WebSocketManager = Depends(get_ws),
    tele: TelemetriaService = Depends(get_telemetria),
) -> None:
    await manager.conectar(ws)

    if tele.ultima_telemetria is not None:
        try:
            await ws.send_json(tele.ultima_telemetria.model_dump())
        except Exception:
            pass

    try:
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        await manager.desconectar(ws)
