from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    APP_NAME: str = "H-DROP Backend"
    APP_VERSION: str = "3.1.0"
    DEBUG: bool = False

    MQTT_BROKER: str = "broker.hivemq.com"
    MQTT_PORT: int = 1883
    MQTT_CLIENT_ID: str = "hdrop_backend_fastapi"
    MQTT_KEEPALIVE: int = 60

    TOPICO_RAW: str = "hdrop/raw"
    TOPICO_TELEMETRIA: str = "hdrop/telemetria"
    TOPICO_COMANDO: str = "hdrop/comando"

    # Calibração do magnetômetro — validada em campo, não alterar sem recalibrar.
    MAG_OFFSET_X: int = 1648
    MAG_OFFSET_Y: int = 119

    CORS_ORIGINS: list[str] = ["http://localhost:4200", "http://127.0.0.1:4200"]


settings = Settings()
