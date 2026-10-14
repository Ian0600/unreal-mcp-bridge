from __future__ import annotations

from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    # UE plugin listener address.
    host: str = "127.0.0.1"
    # UE plugin listener port.
    port: int = 30100
    # TCP connect timeout in seconds.
    connect_timeout: float = 5.0
    # Per-request response timeout in seconds.
    request_timeout: float = 30.0
    # False (default): open a short-lived connection per call and close it right after the
    # reply, so nothing holds the UE listener and several clients/calls can run at once.
    # True: keep one connection open and reuse it (UMCP_PERSISTENT=true).
    persistent: bool = False
    # Maximum accepted JSON line length in bytes (must match UE plugin setting).
    max_line_bytes: int = 16777216

    model_config = SettingsConfigDict(
        env_prefix="UMCP_",
        env_file=".env",
        extra="ignore",
    )


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    # Cached singleton; call get_settings.cache_clear() in tests to reset.
    return Settings()
