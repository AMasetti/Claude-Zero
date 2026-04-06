#!/usr/bin/env python3
"""
flipper_hook.py — Claude Code pre-execution gate via Flipper Zero

Import this module and call request_approval() before executing dangerous
actions. The function sends the action description to the bridge daemon
which forwards it to the Flipper Zero for physical approval.

Fail-open:  bridge unavailable → return True  (Claude works without Flipper)
Fail-closed: timeout / denied  → return False (deny for safety)
"""

import os
import re
import json
import socket
import uuid
import logging
from urllib.parse import urlparse

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SOCKET_PATH = "/tmp/flipper_claude.sock"
REQUEST_TIMEOUT = 35  # slightly > bridge's 30s APPROVAL_TIMEOUT

# Domains that do NOT require approval for web_fetch
WHITELISTED_DOMAINS = {
    "api.anthropic.com",
    "docs.anthropic.com",
    "raw.githubusercontent.com",
    "pypi.org",
    "registry.npmjs.org",
}

# Bash patterns that require approval
BASH_DANGEROUS_PATTERNS = [
    r"\brm\b",
    r"\bsudo\b",
    r"\bcurl\b",
    r"\bwget\b",
    r"\bpip\s+install\b",
    r"\bnpm\s+install\b",
    r"\byarn\s+add\b",
    r"\bgit\s+push\b",
    r"\bchmod\b",
    r"\bchown\b",
    r"\bdd\b",
    r"\bmkfs\b",
    r"\bkill\b",
    r"\bpkill\b",
    r"\bsystemctl\b",
    r"\blaunchctl\b",
]

_COMPILED_PATTERNS = [re.compile(p) for p in BASH_DANGEROUS_PATTERNS]

log = logging.getLogger("flipper_hook")

# ---------------------------------------------------------------------------
# Socket client
# ---------------------------------------------------------------------------

def _send_request(payload: dict) -> dict:
    """
    Send a JSON request to the bridge over the Unix socket.
    Returns the parsed response dict.
    Raises ConnectionRefusedError / FileNotFoundError if bridge is down.
    """
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(REQUEST_TIMEOUT)
            s.connect(SOCKET_PATH)
            msg = (json.dumps(payload) + "\n").encode("utf-8")
            s.sendall(msg)
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
            line = buf.split(b"\n")[0]
            return json.loads(line.decode("utf-8"))
    except (ConnectionRefusedError, FileNotFoundError):
        raise
    except socket.timeout:
        return {"error": "timeout"}
    except Exception as e:
        log.debug("Socket error: %s", e)
        return {"error": str(e)}


def _notify(action: str, kind: str | None = None, message: str = "") -> dict:
    payload: dict = {
        "id": str(uuid.uuid4()),
        "action": action,
    }
    if kind:
        payload["type"] = kind
    if message:
        payload["message"] = message[:120]
    return payload

# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def request_approval(kind: str, message: str) -> bool:
    """
    Request physical approval from Flipper Zero.

    Args:
        kind:    Category label shown in logs: "bash", "write_file",
                 "web_fetch", or "destructive"
        message: Human-readable description of the action (≤120 chars)

    Returns:
        True  → proceed with the action
        False → abort the action

    Failure modes:
        Bridge unavailable (FAIL OPEN)  → True  (Flipper is optional)
        Timeout (FAIL CLOSED)           → False
        Denied / cancelled              → False
    """
    message = message[:120]
    log.info("Requesting approval: kind=%s msg=%r", kind, message)

    payload = _notify("NOTIFY", "ALERT", message)
    payload["kind"] = kind

    try:
        result = _send_request(payload)
    except (ConnectionRefusedError, FileNotFoundError):
        log.info("Bridge unavailable — fail open, proceeding")
        return True

    if "error" in result:
        err = result["error"]
        if err == "timeout":
            log.warning("Approval timed out — denying")
            return False
        if err == "bridge_unavailable":
            log.info("Bridge unavailable (reported) — fail open")
            return True
        log.warning("Bridge error %r — denying", err)
        return False

    approved = result.get("approved", False)
    reason = result.get("reason", "")
    log.info("Approval result: approved=%s reason=%r", approved, reason)
    return bool(approved)


def notify_state(state: str) -> None:
    """
    Send a non-blocking state update to the Flipper (IDLE/THINK/WAIT).
    Swallows all errors — purely cosmetic.

    state: "idle" | "think" | "wait"
    """
    action = state.upper()
    if action not in ("IDLE", "THINK", "WAIT"):
        return
    try:
        _send_request({"action": action, "id": str(uuid.uuid4())})
    except Exception:
        pass


def report_progress(percent: int, message: str = "") -> None:
    """
    Send a progress update to the Flipper (non-blocking, fire-and-forget).
    Shows a progress bar in the WAITING state on the Flipper screen.

    percent: 0-100
    message: short status text (≤80 chars)
    """
    pct = max(0, min(100, int(percent)))
    payload = {
        "id": str(uuid.uuid4()),
        "action": "PROGRESS",
        "percent": pct,
        "message": str(message)[:80],
    }
    try:
        _send_request(payload)
    except Exception:
        pass  # progress is best-effort

# ---------------------------------------------------------------------------
# Interception predicates
# ---------------------------------------------------------------------------

def should_intercept_bash(command: str) -> bool:
    """Return True if this bash command requires Flipper approval."""
    return any(p.search(command) for p in _COMPILED_PATTERNS)


def should_intercept_write(path: str, cwd: str | None = None) -> bool:
    """
    Return True if writing to `path` requires Flipper approval.
    Intercepts writes outside the current working directory.
    """
    if cwd is None:
        cwd = os.getcwd()
    abs_path = os.path.realpath(os.path.abspath(path))
    abs_cwd = os.path.realpath(os.path.abspath(cwd))
    # Allow writes inside cwd or its subdirectories
    return not abs_path.startswith(abs_cwd + os.sep) and abs_path != abs_cwd


def should_intercept_fetch(url: str) -> bool:
    """Return True if fetching `url` requires Flipper approval."""
    try:
        host = urlparse(url).netloc.lower().split(":")[0]  # strip port
        # Check exact match and subdomain match
        for domain in WHITELISTED_DOMAINS:
            if host == domain or host.endswith("." + domain):
                return False
        return True
    except Exception:
        return True  # unknown URLs require approval

# ---------------------------------------------------------------------------
# Convenience: check-and-gate
# ---------------------------------------------------------------------------

def gate_bash(command: str) -> bool:
    """
    Check if command needs approval and request it if so.
    Returns True if Claude should proceed, False to abort.
    """
    if not should_intercept_bash(command):
        return True
    return request_approval("bash", f"bash: {command}")


def gate_write(path: str, cwd: str | None = None) -> bool:
    """
    Check if writing to path needs approval and request it if so.
    Returns True if Claude should proceed, False to abort.
    """
    if not should_intercept_write(path, cwd):
        return True
    return request_approval("write_file", f"write: {path}")


def gate_fetch(url: str) -> bool:
    """
    Check if fetching url needs approval and request it if so.
    Returns True if Claude should proceed, False to abort.
    """
    if not should_intercept_fetch(url):
        return True
    return request_approval("web_fetch", f"fetch: {url}")


# ---------------------------------------------------------------------------
# CLI: quick test
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys
    logging.basicConfig(level=logging.DEBUG)

    print("Flipper Hook — quick test")
    print(f"Socket: {SOCKET_PATH}")
    print()

    tests = [
        ("bash", "echo hello"),
        ("bash", "rm -rf /tmp/test"),
        ("bash", "sudo apt-get update"),
        ("write", "/tmp/testfile"),
        ("write", os.path.join(os.getcwd(), "local.txt")),
        ("fetch", "https://api.anthropic.com/v1/messages"),
        ("fetch", "https://example.com/page"),
    ]

    for test_type, value in tests:
        if test_type == "bash":
            needs = should_intercept_bash(value)
            print(f"bash  {needs!s:5} intercept: {value!r}")
        elif test_type == "write":
            needs = should_intercept_write(value)
            print(f"write {needs!s:5} intercept: {value!r}")
        elif test_type == "fetch":
            needs = should_intercept_fetch(value)
            print(f"fetch {needs!s:5} intercept: {value!r}")

    print()
    print("Run with bridge active to test actual approval flow:")
    print("  python3 flipper_hook.py approve")

    if len(sys.argv) > 1 and sys.argv[1] == "approve":
        print("\nRequesting test approval...")
        result = request_approval("bash", "rm -rf /tmp/test_approval")
        print(f"Result: {'APPROVED' if result else 'DENIED'}")
