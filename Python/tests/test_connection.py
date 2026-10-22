"""UnrealConnection IO-failure recovery, driven by a scripted fake socket instead of a live editor."""

from __future__ import annotations

import json
import socket

import pytest

from unreal_mcp_bridge.config import Settings
from unreal_mcp_bridge.connection import MCPRemoteError, UnrealConnection


class FakeSocket:
    """Hands out queued recv results; an Exception instance in the queue is raised instead."""

    def __init__(self, chunks: list) -> None:
        self._chunks = list(chunks)
        self.closed = False

    def sendall(self, data: bytes) -> None:
        pass

    def settimeout(self, value) -> None:
        pass

    def recv(self, size: int) -> bytes:
        item = self._chunks.pop(0) if self._chunks else b""
        if isinstance(item, Exception):
            raise item
        return item

    def close(self) -> None:
        self.closed = True


def reply(request_id: int, result: dict) -> bytes:
    return (json.dumps({"jsonrpc": "2.0", "id": request_id, "result": result}) + "\n").encode()


def make_conn(monkeypatch, sockets: list[FakeSocket], max_line_bytes: int = 64) -> UnrealConnection:
    conn = UnrealConnection(Settings(max_line_bytes=max_line_bytes))
    queue = list(sockets)

    def fake_connect() -> None:
        conn._sock = queue.pop(0)
        conn._recv_buffer = b""

    monkeypatch.setattr(conn, "connect", fake_connect)
    return conn


def test_oversized_line_does_not_poison_the_next_call(monkeypatch):
    # The bug: the buffer stayed at max_line_bytes after the error, so every later call failed the
    # size check before reading — even on a brand-new socket.
    first = FakeSocket([b"x" * 100])
    second = FakeSocket([reply(2, {"ok": True})])
    conn = make_conn(monkeypatch, [first, second])

    with pytest.raises(ConnectionError, match="exceeded"):
        conn.call("asset.list", {})
    assert conn._recv_buffer == b""
    assert first.closed

    assert conn.call("asset.list", {}) == {"ok": True}


def test_timeout_discards_partial_reply(monkeypatch):
    # Half a reply then a timeout: that half must not be glued onto the next response.
    first = FakeSocket([b'{"jsonrpc":"2.0","id":1,"res', socket.timeout()])
    second = FakeSocket([reply(2, {"ok": True})])
    conn = make_conn(monkeypatch, [first, second], max_line_bytes=4096)

    with pytest.raises(TimeoutError):
        conn.call("sequencer.bake_anim_sequence", {})
    assert conn._recv_buffer == b""

    assert conn.call("asset.list", {}) == {"ok": True}


def test_remote_error_text_carries_data(monkeypatch):
    error = {"jsonrpc": "2.0", "id": 1, "error": {
        "code": -32603,
        "message": "GameThread dispatch timed out",
        "data": {"modal_title": "Reload Packages"},
    }}
    conn = make_conn(monkeypatch, [FakeSocket([(json.dumps(error) + "\n").encode()])], max_line_bytes=4096)

    with pytest.raises(MCPRemoteError) as info:
        conn.call("editor.python_exec", {"script": "pass"})
    assert info.value.data == {"modal_title": "Reload Packages"}
    assert "Reload Packages" in str(info.value)
