from fastapi import APIRouter

from src.api.v1.routes import comando, health, telemetria

router_v1 = APIRouter(prefix="/api/v1")
router_v1.include_router(health.router)
router_v1.include_router(comando.router)
router_v1.include_router(telemetria.router)
