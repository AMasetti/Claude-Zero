#!/usr/bin/env python3
"""
Claude-Zero Mac Bridge
Bridges Flipper Zero USB serial <-> Unix socket for Claude Code skill.

Usage:
    python3 bridge.py              # foreground mode
    python3 bridge.py --daemon     # background daemon
    python3 bridge.py --mock       # mock mode (no hardware needed)
    python3 bridge.py --mock --interactive  # mock mode with stdin responses
"""

import os
import sys
import socket
import threading
import time
import json
import logging
import select
import struct
import uuid
from pathlib import Path

import click

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SOCKET_PATH = "/tmp/flipper_claude.sock"
PID_PATH = "/tmp/flipper_claude.pid"
LOG_PATH = str(Path.home() / ".claude_flipper.log")
BAUD_RATE = 115200
FLIPPER_VID = 0x0483
FLIPPER_PID = 0x5740
APPROVAL_TIMEOUT = 30  # seconds
RECONNECT_DELAY = 2    # seconds

# ---------------------------------------------------------------------------
# Logging setup
# ---------------------------------------------------------------------------

def setup_logging(daemon: bool, verbose: bool) -> None:
    level = logging.DEBUG if verbose else logging.INFO
    handlers = [logging.FileHandler(LOG_PATH)]
    if not daemon:
        handlers.append(logging.StreamHandler(sys.stderr))
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)-8s %(message)s",
        handlers=handlers,
    )

log = logging.getLogger("bridge")

# ---------------------------------------------------------------------------
# Flipper Zero detection
# ---------------------------------------------------------------------------

def find_flipper_port() -> str | None:
    """Return serial port path for a connected Flipper Zero, or None."""
    try:
        from serial.tools import list_ports
        for port in list_ports.comports():
            if port.vid == FLIPPER_VID and port.pid == FLIPPER_PID:
                log.debug("Found Flipper at %s (vid=%04x pid=%04x)", port.device, port.vid, port.pid)
                return port.device
        # Fallback: name-based match for systems where VID/PID not reported
        for port in list_ports.comports():
            desc = (port.description or "").lower()
            name = (port.device or "").lower()
            if "flipper" in desc or "flipper" in name:
                log.debug("Found Flipper by name at %s", port.device)
                return port.device
    except Exception as e:
        log.warning("Port scan error: %s", e)
    return None

# ---------------------------------------------------------------------------
# Bridge state
# ---------------------------------------------------------------------------

class PendingRequest:
    """Tracks a single in-flight approval request."""
    def __init__(self):
        self.event = threading.Event()
        self.approved: bool = False
        self.reason: str = "timeout"

class BridgeState:
    def __init__(self):
        self.ser = None          # serial.Serial | None
        self.ser_lock = threading.Lock()
        self.pending: dict[str, PendingRequest] = {}
        self.pending_lock = threading.Lock()
        self.running = True
        self.mock_mode = False
        self.mock_interactive = False

# ---------------------------------------------------------------------------
# Serial write helper
# ---------------------------------------------------------------------------

def serial_write(state: BridgeState, command: str) -> bool:
    """Send a newline-terminated command to Flipper. Returns False if disconnected."""
    line = (command.rstrip("\n") + "\n").encode()
    with state.ser_lock:
        if state.ser is None:
            log.warning("Serial not connected, dropping: %s", command.strip())
            return False
        try:
            state.ser.write(line)
            state.ser.flush()
            log.debug("→ Flipper: %s", command.strip())
            return True
        except Exception as e:
            log.error("Serial write error: %s", e)
            state.ser = None
            return False

# ---------------------------------------------------------------------------
# Serial reader thread (real hardware)
# ---------------------------------------------------------------------------

def serial_reader_thread(state: BridgeState) -> None:
    """Continuously reads lines from Flipper, signals pending requests."""
    import serial

    while state.running:
        port = find_flipper_port()
        if port is None:
            log.info("Flipper not found, retrying in %ds...", RECONNECT_DELAY)
            time.sleep(RECONNECT_DELAY)
            continue

        try:
            ser = serial.Serial(port, BAUD_RATE, timeout=1)
            with state.ser_lock:
                state.ser = ser
            log.info("Connected to Flipper at %s", port)

            line_buf = b""
            while state.running:
                try:
                    chunk = ser.read(64)
                    if not chunk:
                        continue
                    line_buf += chunk
                    while b"\n" in line_buf:
                        line, line_buf = line_buf.split(b"\n", 1)
                        response = line.strip().decode("utf-8", errors="ignore")
                        if response:
                            handle_flipper_response(state, response)
                except serial.SerialException as e:
                    log.warning("Serial read error: %s", e)
                    break

        except Exception as e:
            log.warning("Serial connection error: %s", e)
        finally:
            with state.ser_lock:
                if state.ser is not None:
                    try:
                        state.ser.close()
                    except Exception:
                        pass
                    state.ser = None
            log.info("Disconnected from Flipper, reconnecting in %ds...", RECONNECT_DELAY)
            if state.running:
                time.sleep(RECONNECT_DELAY)

# ---------------------------------------------------------------------------
# Mock serial reader thread (no hardware)
# ---------------------------------------------------------------------------

def mock_serial_reader_thread(state: BridgeState) -> None:
    """
    Mock reader that auto-approves or reads from stdin.
    Checks for new pending requests and resolves them.
    """
    auto_delay = float(os.environ.get("MOCK_AUTO_APPROVE_DELAY", "2"))
    log.info("Mock serial reader started (auto_delay=%.1fs, interactive=%s)",
             auto_delay, state.mock_interactive)

    while state.running:
        # Find any pending requests and resolve them
        with state.pending_lock:
            req_ids = list(state.pending.keys())

        for req_id in req_ids:
            with state.pending_lock:
                if req_id not in state.pending:
                    continue
                req = state.pending[req_id]

            if state.mock_interactive:
                log.info("[MOCK] Pending approval for %s — type OK/DENY/CANCEL:", req_id[:8])
                # Non-blocking stdin check
                rlist, _, _ = select.select([sys.stdin], [], [], 0)
                if rlist:
                    line = sys.stdin.readline().strip().upper()
                    handle_flipper_response(state, line)
            else:
                # Auto-approve after delay
                time.sleep(auto_delay)
                log.info("[MOCK] Auto-approving request %s", req_id[:8])
                handle_flipper_response(state, "OK")

        time.sleep(0.1)

# ---------------------------------------------------------------------------
# Response handler
# ---------------------------------------------------------------------------

def handle_flipper_response(state: BridgeState, response: str) -> None:
    """Process a response line received from the Flipper."""
    log.debug("← Flipper: %s", response)

    with state.pending_lock:
        if not state.pending:
            log.debug("Response %r with no pending requests", response)
            return

        # Signal the oldest pending request (FIFO)
        req_id = next(iter(state.pending))
        req = state.pending.pop(req_id)

    if response == "OK":
        req.approved = True
        req.reason = ""
    elif response == "DENY":
        req.approved = False
        req.reason = "denied"
    elif response == "CANCEL":
        req.approved = False
        req.reason = "cancelled"
    else:
        log.warning("Unknown Flipper response: %r", response)
        req.approved = False
        req.reason = f"unknown:{response}"

    req.event.set()

# ---------------------------------------------------------------------------
# Request dispatcher
# ---------------------------------------------------------------------------

def dispatch_request(state: BridgeState, payload: dict) -> dict:
    """
    Handle a parsed JSON request from a Unix socket client.
    Returns a JSON-serialisable response dict.
    """
    action = payload.get("action", "").upper()
    req_id = payload.get("id") or str(uuid.uuid4())

    log.info("Request action=%s id=%s", action, req_id[:8])

    if action == "NOTIFY":
        # Support both "kind" (new) and "type" (legacy) fields.
        # Approval kinds: BASH, TOOL, PERM (new protocol), ALERT (legacy).
        kind = payload.get("kind", payload.get("type", "BASH")).upper()
        message = str(payload.get("message", ""))[:120]

        # Forward to Flipper using new serial protocol
        cmd = f"NOTIFY:{kind}:{message}"
        serial_write(state, cmd)

        # All alert kinds require physical approval
        if kind in ("BASH", "TOOL", "PERM", "ALERT"):
            req = PendingRequest()
            with state.pending_lock:
                state.pending[req_id] = req

            fired = req.event.wait(timeout=APPROVAL_TIMEOUT)
            if not fired:
                with state.pending_lock:
                    state.pending.pop(req_id, None)
                log.warning("Approval timeout for %s", req_id[:8])
                serial_write(state, "IDLE")
                return {"id": req_id, "approved": False, "reason": "timeout"}

            return {"id": req_id, "approved": req.approved, "reason": req.reason}
        else:
            # Non-approval NOTIFY (e.g. OK/DENY display): no wait
            return {"id": req_id, "ok": True}

    elif action in ("IDLE", "THINK", "WAIT"):
        serial_write(state, action)
        return {"id": req_id, "ok": True}

    elif action == "PROGRESS":
        pct = max(0, min(100, int(payload.get("percent", 0))))
        message = str(payload.get("message", ""))[:80]
        serial_write(state, f"PROGRESS:{pct}:{message}")
        return {"id": req_id, "ok": True}

    else:
        log.warning("Unknown action: %s", action)
        return {"id": req_id, "error": f"unknown_action:{action}"}

# ---------------------------------------------------------------------------
# Unix socket server
# ---------------------------------------------------------------------------

def run_socket_server(state: BridgeState) -> None:
    """Main loop: accept Unix socket connections and dispatch requests."""

    # Clean up stale socket
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o600)
    server.listen(8)
    server.setblocking(False)

    log.info("Listening on %s", SOCKET_PATH)

    clients: list[socket.socket] = []
    client_bufs: dict[int, bytes] = {}

    try:
        while state.running:
            read_fds = [server] + clients
            try:
                readable, _, _ = select.select(read_fds, [], [], 1.0)
            except (ValueError, OSError):
                break

            for fd in readable:
                if fd is server:
                    try:
                        conn, _ = server.accept()
                        conn.setblocking(False)
                        clients.append(conn)
                        client_bufs[id(conn)] = b""
                        log.debug("New client connected")
                    except OSError:
                        pass
                else:
                    try:
                        data = fd.recv(4096)
                    except OSError:
                        data = b""

                    if not data:
                        # Client disconnected
                        clients.remove(fd)
                        client_bufs.pop(id(fd), None)
                        fd.close()
                        continue

                    client_bufs[id(fd)] += data

                    # Process complete newline-delimited messages
                    while b"\n" in client_bufs[id(fd)]:
                        line, client_bufs[id(fd)] = client_bufs[id(fd)].split(b"\n", 1)
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            payload = json.loads(line.decode("utf-8"))
                        except (json.JSONDecodeError, UnicodeDecodeError) as e:
                            log.warning("Bad JSON from client: %s", e)
                            resp = json.dumps({"error": "parse_error"}) + "\n"
                            try:
                                fd.sendall(resp.encode())
                            except OSError:
                                pass
                            continue

                        try:
                            result = dispatch_request(state, payload)
                        except Exception as e:
                            log.exception("Dispatch error: %s", e)
                            result = {"error": "internal_error", "detail": str(e)}

                        resp = json.dumps(result) + "\n"
                        try:
                            fd.sendall(resp.encode())
                        except OSError:
                            log.warning("Failed to send response to client")
    finally:
        for c in clients:
            try:
                c.close()
            except OSError:
                pass
        server.close()
        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)
        log.info("Socket server stopped")

# ---------------------------------------------------------------------------
# Daemon helpers
# ---------------------------------------------------------------------------

def daemonize() -> None:
    """Double-fork to become a Unix daemon."""
    # First fork
    pid = os.fork()
    if pid > 0:
        sys.exit(0)

    os.setsid()

    # Second fork
    pid = os.fork()
    if pid > 0:
        sys.exit(0)

    # Redirect stdio
    devnull = os.open(os.devnull, os.O_RDWR)
    os.dup2(devnull, sys.stdin.fileno())
    os.dup2(devnull, sys.stdout.fileno())
    os.dup2(devnull, sys.stderr.fileno())
    os.close(devnull)

    # Write PID file
    with open(PID_PATH, "w") as f:
        f.write(str(os.getpid()))

# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

@click.command()
@click.option("--daemon", is_flag=True, help="Run as background daemon")
@click.option("--mock", is_flag=True, help="Mock mode: no Flipper hardware required")
@click.option("--interactive", "mock_interactive", is_flag=True,
              help="In mock mode, read OK/DENY/CANCEL from stdin")
@click.option("--verbose", is_flag=True, help="Debug logging")
def main(daemon: bool, mock: bool, mock_interactive: bool, verbose: bool) -> None:
    """Claude-Zero bridge: connects Flipper Zero to Claude Code via Unix socket."""

    if daemon and mock:
        click.echo("Warning: --daemon and --mock together will daemon without hardware")

    if daemon:
        daemonize()

    setup_logging(daemon, verbose)
    log.info("Starting Claude-Zero bridge (mock=%s, daemon=%s)", mock, daemon)

    state = BridgeState()
    state.mock_mode = mock
    state.mock_interactive = mock_interactive

    # Start serial thread (real or mock)
    if mock:
        reader = threading.Thread(
            target=mock_serial_reader_thread, args=(state,), daemon=True, name="MockSerial"
        )
    else:
        reader = threading.Thread(
            target=serial_reader_thread, args=(state,), daemon=True, name="SerialReader"
        )
    reader.start()

    try:
        run_socket_server(state)
    except KeyboardInterrupt:
        log.info("Interrupted, shutting down")
    finally:
        state.running = False
        reader.join(timeout=3)
        if os.path.exists(PID_PATH):
            try:
                os.unlink(PID_PATH)
            except OSError:
                pass
        log.info("Bridge stopped")


if __name__ == "__main__":
    main()
