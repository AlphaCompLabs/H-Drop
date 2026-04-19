import asyncio
import json
from typing import Optional

import aiomqtt

from src.core.config import settings
from src.core.logging import get_logger
from src.schemas.comando import ComandoDestino
from src.schemas.telemetria import RawTelemetria
from src.services.telemetria_service import telemetria_service
from src.services.websocket_manager import websocket_manager

logger = get_logger(__name__)

RECONNECT_DELAY_S = 5


class MQTTService:
    """Cliente MQTT assíncrono rodando em task de background do FastAPI."""

    def __init__(self) -> None:
        self._cliente: Optional[aiomqtt.Client] = None
        self._conectado: bool = False

    @property
    def conectado(self) -> bool:
        return self._conectado

    async def executar(self) -> None:
        while True:
            try:
                async with aiomqtt.Client(
                    hostname=settings.MQTT_BROKER,
                    port=settings.MQTT_PORT,
                    identifier=settings.MQTT_CLIENT_ID,
                    keepalive=settings.MQTT_KEEPALIVE,
                ) as cliente:
                    self._cliente = cliente
                    self._conectado = True
                    logger.info("MQTT conectado em %s:%d", settings.MQTT_BROKER, settings.MQTT_PORT)

                    await cliente.subscribe(settings.TOPICO_RAW)
                    await cliente.subscribe(settings.TOPICO_COMANDO)
                    logger.info("Subscribe: %s, %s", settings.TOPICO_RAW, settings.TOPICO_COMANDO)

                    async for mensagem in cliente.messages:
                        await self._tratar_mensagem(mensagem)

            except asyncio.CancelledError:
                logger.info("MQTT loop cancelado")
                raise
            except aiomqtt.MqttError as exc:
                logger.error("MQTT erro: %s. Reconectando em %ds…", exc, RECONNECT_DELAY_S)
            except Exception as exc:
                logger.exception("MQTT erro inesperado: %s", exc)
            finally:
                self._conectado = False
                self._cliente = None

            await asyncio.sleep(RECONNECT_DELAY_S)

    async def _tratar_mensagem(self, mensagem: aiomqtt.Message) -> None:
        topico = str(mensagem.topic)
        try:
            conteudo = mensagem.payload.decode("utf-8") if isinstance(mensagem.payload, (bytes, bytearray)) else str(mensagem.payload)
        except Exception as exc:
            logger.warning("Payload não decodificável em %s: %s", topico, exc)
            return

        if topico == settings.TOPICO_RAW:
            await self._tratar_raw(conteudo)
        elif topico == settings.TOPICO_COMANDO:
            await self._tratar_comando_eco(conteudo)
        else:
            logger.warning("Tópico inesperado [%s]: %s", topico, conteudo)

    async def _tratar_raw(self, conteudo: str) -> None:
        try:
            raw = RawTelemetria.model_validate_json(conteudo)
        except Exception as exc:
            logger.warning("Raw inválido: %s | payload=%s", exc, conteudo)
            return

        processada = telemetria_service.processar_raw(raw)
        payload = processada.model_dump()

        await self.publicar(settings.TOPICO_TELEMETRIA, payload)
        await websocket_manager.broadcast(payload)

    async def _tratar_comando_eco(self, conteudo: str) -> None:
        """Observa comandos publicados para sincronizar o 'último destino' interno."""
        try:
            destino = ComandoDestino.model_validate_json(conteudo)
            telemetria_service.registrar_destino(destino)
        except Exception as exc:
            logger.warning("Comando inválido no tópico: %s | payload=%s", exc, conteudo)

    async def publicar(self, topico: str, payload: dict, qos: int = 0) -> bool:
        if not self._cliente or not self._conectado:
            logger.warning("Publish ignorado — MQTT desconectado (topico=%s)", topico)
            return False
        try:
            await self._cliente.publish(topico, json.dumps(payload), qos=qos)
            return True
        except Exception as exc:
            logger.error("Falha ao publicar em %s: %s", topico, exc)
            return False


mqtt_service = MQTTService()
