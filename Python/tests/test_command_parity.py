"""Asserts the Python tool layer and the C++ command registry describe the same method set.

The two halves of this repository drift apart in one direction and one direction only: a command
gets added to the plugin, the editor answers it, and the MCP tool that would let a client reach it
is never written. Nothing else notices — the plugin is happy, the server starts, the method is just
invisible.

So the check is textual and deliberately dumb: every `Registry.Register(TEXT("x.y"))` in the plugin
sources must have exactly one `conn.call("x.y")` in `unreal_mcp_bridge/tools/`, and the reverse.
Neither side has to be importable or running for this to work, and a stale copy of the plugin
sources fails the same way a missing tool does.
"""

from __future__ import annotations

import re
from collections import Counter
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
COMMANDS_DIR = REPO_ROOT / "Plugin" / "UnrealMCPBridge" / "Source" / "UnrealMCPBridge" / "Private" / "Commands"
TOOLS_DIR = REPO_ROOT / "Python" / "unreal_mcp_bridge" / "tools"

REGISTER_RE = re.compile(r'Registry\.Register\(\s*TEXT\("([a-z_0-9]+\.[a-z_0-9]+)"\)')
CALL_RE = re.compile(r'conn\.call\(\s*"([a-z_0-9]+\.[a-z_0-9]+)"')


def _scan(directory: Path, pattern: re.Pattern[str], suffix: str) -> Counter[str]:
    counts: Counter[str] = Counter()
    for path in sorted(directory.rglob(f"*{suffix}")):
        counts.update(pattern.findall(path.read_text(encoding="utf-8")))
    return counts


@pytest.fixture(scope="module")
def registered() -> Counter[str]:
    if not COMMANDS_DIR.is_dir():
        pytest.skip(f"plugin sources not present at {COMMANDS_DIR}")
    counts = _scan(COMMANDS_DIR, REGISTER_RE, ".cpp")
    assert counts, "no Registry.Register calls found — the scan pattern is wrong, not the code"
    return counts


@pytest.fixture(scope="module")
def wrapped() -> Counter[str]:
    counts = _scan(TOOLS_DIR, CALL_RE, ".py")
    assert counts, "no conn.call sites found — the scan pattern is wrong, not the code"
    return counts


def test_every_command_has_a_tool(registered: Counter[str], wrapped: Counter[str]) -> None:
    missing = sorted(set(registered) - set(wrapped))
    assert not missing, (
        "plugin commands with no MCP tool wrapper — an MCP client cannot reach these:\n  "
        + "\n  ".join(missing)
    )


def test_every_tool_has_a_command(registered: Counter[str], wrapped: Counter[str]) -> None:
    extra = sorted(set(wrapped) - set(registered))
    assert not extra, (
        "MCP tools calling methods the plugin does not register — these fail with MethodNotFound:\n  "
        + "\n  ".join(extra)
    )


def test_no_duplicate_registrations(registered: Counter[str]) -> None:
    # The registry logs a warning and overwrites on a duplicate, so the second handler silently
    # wins and the first one is dead code.
    duplicates = sorted(name for name, count in registered.items() if count > 1)
    assert not duplicates, f"method registered more than once: {duplicates}"


def test_no_duplicate_wrappers(wrapped: Counter[str]) -> None:
    # Two tools calling one method is legal but almost always a copy-paste; FastMCP would also
    # reject the duplicate tool name if they were named alike.
    duplicates = sorted(name for name, count in wrapped.items() if count > 1)
    assert not duplicates, f"method wrapped by more than one tool: {duplicates}"
