from __future__ import annotations

import itertools
import json
import socket
import threading
from typing import Any

from .config import Settings


class MCPRemoteError(Exception):
    """Raised when the UE plugin returns a JSON-RPC error object."""

    def __init__(self, code: int, message: str, data: Any = None) -> None:
        super().__init__(f"[{code}] {message}")
        self.code = code
        self.message = message
        self.data = data


class UnrealConnection:
    """TCP client for the UE MCP Bridge plugin.

    By default every call opens its own short-lived connection and closes it after the reply,
    so the UE listener is only occupied while a request is in flight and concurrent calls (from
    threads or from several MCP servers) do not wait on each other. With
    ``Settings.persistent`` one connection is kept open and calls are serialized over it.
    """

    def __init__(self, settings: Settings) -> None:
        self._settings = settings
        self._sock: socket.socket | None = None
        self._lock = threading.Lock()
        self._id_counter = itertools.count(1)
        # Buffer for partial lines received across multiple recv calls (persistent mode only).
        self._recv_buffer = b""

    def _open(self) -> socket.socket:
        """Open a TCP connection to the UE plugin listener. Raises on failure."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self._settings.connect_timeout)
        try:
            sock.connect((self._settings.host, self._settings.port))
        except OSError as exc:
            sock.close()
            raise ConnectionError(
                f"Cannot connect to UE plugin at "
                f"{self._settings.host}:{self._settings.port}: {exc}"
            ) from exc
        # Switch to blocking mode with request timeout for subsequent IO.
        sock.settimeout(self._settings.request_timeout)
        return sock

    def connect(self) -> None:
        """Open the persistent connection (persistent mode). Raises on failure."""
        self._sock = self._open()
        self._recv_buffer = b""

    def _ensure_connected(self) -> None:
        if self._sock is None:
            self.connect()

    def call(
        self,
        method: str,
        params: dict[str, Any] | None = None,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        """Send a JSON-RPC request and return the result dict.

        Raises MCPRemoteError when the plugin returns an error response.
        Raises ConnectionError / TimeoutError on IO failures.
        """
        request_id = next(self._id_counter)
        payload: dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
        }
        if params is not None:
            payload["params"] = params
        encoded = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")

        if self._settings.persistent:
            with self._lock:
                self._ensure_connected()
                assert self._sock is not None
                response_line = self._exchange(encoded, timeout, persistent=True)
        else:
            sock = self._open()
            try:
                response_line = self._exchange(encoded, timeout, persistent=False, sock=sock)
            finally:
                try:
                    sock.close()
                except OSError:
                    pass

        # Parse and validate the response.
        try:
            response = json.loads(response_line)
        except json.JSONDecodeError as exc:
            raise ConnectionError(f"Invalid JSON from server: {exc}") from exc

        if response.get("id") != request_id:
            raise ConnectionError(
                f"Response id mismatch: expected {request_id}, got {response.get('id')}"
            )

        if "error" in response:
            err = response["error"]
            raise MCPRemoteError(
                code=err.get("code", 0),
                message=err.get("message", "Unknown error"),
                data=err.get("data"),
            )

        return response.get("result") or {}

    def _exchange(
        self,
        encoded: bytes,
        timeout: float | None,
        persistent: bool,
        sock: socket.socket | None = None,
    ) -> str:
        """Send one request line and read one response line."""
        active = self._sock if persistent else sock
        assert active is not None
        try:
            active.sendall(encoded)
        except OSError as exc:
            if persistent:
                self._sock = None
            raise ConnectionError(f"Send failed: {exc}") from exc

        # Override timeout for this call if requested.
        if timeout is not None:
            active.settimeout(timeout)

        line = self._recv_line(active, persistent)

        if timeout is not None and persistent and self._sock is not None:
            # Restore default timeout.
            self._sock.settimeout(self._settings.request_timeout)
        return line

    def _recv_line(self, sock: socket.socket, persistent: bool) -> str:
        """Read bytes until a newline delimiter, respecting max_line_bytes."""
        max_bytes = self._settings.max_line_bytes
        buffer = self._recv_buffer if persistent else b""

        def drop() -> None:
            if persistent:
                self._sock = None

        while b"\n" not in buffer:
            if len(buffer) >= max_bytes:
                drop()
                raise ConnectionError(
                    f"Response line exceeded {max_bytes} bytes without a newline delimiter"
                )
            try:
                chunk = sock.recv(4096)
            except socket.timeout as exc:
                drop()
                raise TimeoutError("Timed out waiting for response from UE plugin") from exc
            except OSError as exc:
                drop()
                raise ConnectionError(f"Receive failed: {exc}") from exc

            if not chunk:
                drop()
                raise ConnectionError("Connection closed by UE plugin while waiting for response")
            buffer += chunk

        newline_pos = buffer.index(b"\n")
        line_bytes = buffer[:newline_pos]
        if persistent:
            self._recv_buffer = buffer[newline_pos + 1:]
        return line_bytes.decode("utf-8")

    def close(self) -> None:
        """Close the persistent connection, if any."""
        with self._lock:
            if self._sock is not None:
                try:
                    self._sock.close()
                except OSError:
                    pass
                self._sock = None
