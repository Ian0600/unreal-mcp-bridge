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
    # Per-request response timeout in seconds. Kept above the plugin's GameThreadDispatchTimeoutSeconds
    # (30): the plugin answers a stuck command at 30s with a diagnostic error (e.g. which modal window
    # is open), and a client that gives up at the same moment never reads it.
    request_timeout: float = 45.0
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
