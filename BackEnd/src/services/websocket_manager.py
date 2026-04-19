import asyncio
from typing import Any

from fastapi import WebSocket

from src.core.logging import get_logger

logger = get_logger(__name__)


class WebSocketManager:
    """Mantém clientes WebSocket conectados e faz broadcast de telemetria."""

    def __init__(self) -> None:
        self._clientes: set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def conectar(self, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self._clientes.add(ws)
        logger.info("WS conectado (%d cliente(s) ativo(s))", len(self._clientes))

    async def desconectar(self, ws: WebSocket) -> None:
        async with self._lock:
            self._clientes.discard(ws)
        logger.info("WS desconectado (%d restante(s))", len(self._clientes))

    async def desconectar_todos(self) -> None:
        async with self._lock:
            clientes = list(self._clientes)
            self._clientes.clear()
        for ws in clientes:
            try:
                await ws.close()
            except Exception:
                pass

    async def broadcast(self, payload: dict[str, Any]) -> None:
        async with self._lock:
            clientes = list(self._clientes)
        if not clientes:
            return
        mortos: list[WebSocket] = []
        for ws in clientes:
            try:
                await ws.send_json(payload)
            except Exception:
                mortos.append(ws)
        if mortos:
            async with self._lock:
                for ws in mortos:
                    self._clientes.discard(ws)

    @property
    def total_clientes(self) -> int:
        return len(self._clientes)


websocket_manager = WebSocketManager()
