import uvicorn
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from src.api.v1.router import router_v1
from src.core.config import settings
from src.core.lifespan import lifespan
from src.core.logging import configurar_logging

configurar_logging(debug=settings.DEBUG)

app = FastAPI(
    title=settings.APP_NAME,
    version=settings.APP_VERSION,
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.CORS_ORIGINS,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(router_v1)


@app.get("/")
async def raiz() -> dict:
    return {
        "app": settings.APP_NAME,
        "versao": settings.APP_VERSION,
        "docs": "/docs",
        "websocket": "/api/v1/ws/telemetria",
    }


if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=settings.DEBUG)
