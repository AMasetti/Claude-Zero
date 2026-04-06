# Claude-Zero

**Physical approval interface for Claude Code actions via Flipper Zero.**

When Claude is about to run a dangerous command (`rm`, `sudo`, `git push`,
etc.), it pauses and sends the action description to your Flipper Zero over
USB. A tamagotchi-style animated character waves its arms. You press OK or
DENY on the device. Claude proceeds only if you approve.

```
Claude Code ──► flipper_hook.py ──► Unix socket ──► bridge.py ──► USB serial ──► Flipper Zero
                                                                                       │
                                                        OK / DENY ◄────────────────────┘
```

---

## Architecture

| Component | File | Role |
|-----------|------|------|
| Flipper app (C) | `flipper_claude/flipper_claude.c` | Animated UI, serial protocol, button input |
| Mac bridge (Python) | `mac_bridge/bridge.py` | Serial ↔ Unix socket broker, daemon |
| Claude skill | `claude_skill/flipper_hook.py` | Pre-execution gate for Claude Code |
| Skill docs | `claude_skill/SKILL.md` | Instructions for Claude Code |
| Progress extension | `claude_skill/PROGRESS_SKILL.md` | Long-task progress tracking |

---

## Prerequisites

- **macOS** (bridge uses launchd; Linux works too but needs manual service setup)
- **Python 3.9+** with `pip`
- **Flipper Zero** running official firmware 0.86+ or Unleashed/RogueMaster
- **ufbt** (micro Flipper Build Tool) — install with `pip3 install ufbt`

---

## Quick Start

```bash
# 1. Clone and run setup
git clone https://github.com/amasetti/claude-zero
cd claude-zero
bash setup.sh

# 2. Build and install the Flipper app (Flipper connected via USB)
cd flipper_claude
ufbt build
ufbt launch   # or copy dist/f7/flipper_claude.fap to SD card manually

# 3. The bridge starts automatically via launchd on macOS.
#    To start manually:
python3 mac_bridge/bridge.py

# 4. Load the skill in your Claude Code session (see Skill Setup below)
```

---

## Flipper App Build & Install

### Build

```bash
cd flipper_claude
ufbt build
```

Output: `dist/f7/flipper_claude.fap`

### Install via USB

```bash
# With Flipper connected via USB-C:
ufbt launch
```

### Install via SD card

1. Copy `dist/f7/flipper_claude.fap` to your Flipper's SD card at:
   `/ext/apps/Tools/flipper_claude.fap`
2. On the Flipper: **Apps → Tools → Claude Approval**

### First run

The app starts in **IDLE** state — the tamagotchi character walks back and
forth. It listens for serial commands from the bridge and responds to button
presses when an approval request arrives.

---

## Bridge Configuration

### Foreground mode

```bash
python3 mac_bridge/bridge.py
python3 mac_bridge/bridge.py --verbose   # debug logging
```

### Daemon mode (manual)

```bash
python3 mac_bridge/bridge.py --daemon
# PID written to /tmp/flipper_claude.pid
# Logs to ~/.claude_flipper.log
```

### launchd (auto-start on login, macOS)

`setup.sh` installs the launchd agent automatically. To manage it manually:

```bash
# Start
launchctl load ~/Library/LaunchAgents/com.claude.flipper.plist

# Stop
launchctl unload ~/Library/LaunchAgents/com.claude.flipper.plist

# Check status
launchctl list | grep flipper

# View logs
tail -f ~/.claude_flipper.log
```

### Mock mode (no hardware)

Test the full approval flow without a Flipper Zero:

```bash
# Auto-approves after 2 seconds
python3 mac_bridge/bridge.py --mock

# Interactive: type OK / DENY / CANCEL in terminal
python3 mac_bridge/bridge.py --mock --interactive

# Control auto-approve delay
MOCK_AUTO_APPROVE_DELAY=5 python3 mac_bridge/bridge.py --mock
```

---

## Claude Code Skill Setup

### Option 1: Reference in your project

Copy or symlink `claude_skill/` into your project and reference `SKILL.md` in
your Claude Code configuration or paste its contents into a system prompt.

### Option 2: Import directly in code

```python
from claude_skill.flipper_hook import gate_bash, gate_write, gate_fetch

# Before dangerous bash
if not gate_bash("rm -rf /tmp/old"):
    raise PermissionError("Denied on Flipper Zero")

# Before writing outside cwd
if not gate_write("/etc/hosts"):
    raise PermissionError("Denied on Flipper Zero")

# Before fetching non-whitelisted URLs
if not gate_fetch("https://example.com/script.sh"):
    raise PermissionError("Denied on Flipper Zero")
```

### Option 3: Load via Claude Code skill mechanism

If your Claude Code setup supports skill directories, point it to
`claude_skill/SKILL.md`.

---

## Serial Protocol Reference

Communication over USB at **115200 baud**, newline-terminated text.

### Host → Flipper

| Command | Description |
|---------|-------------|
| `IDLE\n` | Return to idle walking animation |
| `THINK\n` | Thinking state (thought bubble + ZZZ) |
| `WAIT\n` | Waiting state (spinner) |
| `NOTIFY:ALERT:<msg>\n` | Show approval request, wait for button |
| `NOTIFY:OK:<msg>\n` | Show approved notification |
| `NOTIFY:DENY:<msg>\n` | Show denied notification |
| `PROGRESS:<0-100>:<msg>\n` | Progress bar update (extension) |

### Flipper → Host

| Response | Meaning |
|----------|---------|
| `OK\n` | User pressed Right button (approve) |
| `DENY\n` | User pressed Left button (deny) |
| `CANCEL\n` | User pressed Back button (cancel) |

---

## Unix Socket Protocol Reference

Socket at `/tmp/flipper_claude.sock`. JSON, newline-delimited.

### Client → Bridge

```json
{"id": "uuid", "action": "NOTIFY", "type": "ALERT", "message": "rm -rf /tmp"}
{"id": "uuid", "action": "NOTIFY", "type": "OK", "message": "Command approved"}
{"id": "uuid", "action": "IDLE"}
{"id": "uuid", "action": "THINK"}
{"id": "uuid", "action": "WAIT"}
{"id": "uuid", "action": "PROGRESS", "percent": 42, "message": "Building..."}
```

### Bridge → Client

```json
{"id": "uuid", "approved": true}
{"id": "uuid", "approved": false, "reason": "denied"}
{"id": "uuid", "approved": false, "reason": "timeout"}
{"id": "uuid", "approved": false, "reason": "cancelled"}
{"id": "uuid", "ok": true}
{"id": "uuid", "error": "parse_error"}
```

---

## Flipper Character Animations

| State | Animation |
|-------|-----------|
| IDLE | Character walks left/right, bobs up/down |
| THINKING | Stationary + thought bubble with ZZZ |
| ALERT | Arms wave up/down, "!" above head, border flashes |
| WAITING | Spinning arc above head, optional progress bar |
| APPROVED | Jump + sparkle particles, "APPROVED" banner |
| DENIED | Horizontal shake with decreasing amplitude, "DENIED" banner |

The character is a 14×6 pixel tamagotchi drawn with exact pixel bitmaps.

---

## Progress Tracking Extension

For long-running tasks, load the progress extension skill:

```
# Paste into Claude Code session:
cat claude_skill/PROGRESS_SKILL.md
```

This teaches Claude to call `report_progress()` at milestones:

```python
from claude_skill.flipper_hook import notify_state, report_progress

notify_state("wait")
report_progress(0, "Starting build...")
# ... do work ...
report_progress(50, "Halfway done")
# ... more work ...
report_progress(100, "Complete!")
notify_state("idle")
```

The Flipper shows a filling progress bar in the WAITING state.

---

## Troubleshooting

### Flipper not detected

```bash
# Check if port is visible
python3 -c "from serial.tools import list_ports; [print(p) for p in list_ports.comports()]"
```

If not listed: check USB cable, try another port, ensure Flipper firmware is
up to date, check macOS system settings for USB serial permissions.

### Bridge won't start

```bash
# Check for stale socket
ls -la /tmp/flipper_claude.sock
rm -f /tmp/flipper_claude.sock

# Check for stale PID
cat /tmp/flipper_claude.pid
kill $(cat /tmp/flipper_claude.pid) 2>/dev/null
```

### Approval always times out

- Verify the Flipper app is running (Apps → Tools → Claude Approval)
- Check serial connection: `screen /dev/tty.usbmodemXXXX 115200`
- Type `NOTIFY:ALERT:test` and press Enter — Flipper should show alert
- Type `OK` and verify response

### Claude proceeds without asking

The skill is **fail-open**: if the bridge is not running, Claude proceeds
without approval. This is intentional — the Flipper is optional. To require
approval, ensure the bridge is running before starting Claude Code.

### Permission denied on socket

```bash
chmod 600 /tmp/flipper_claude.sock
```

### Logs

```bash
tail -f ~/.claude_flipper.log
```

---

## File Structure

```
Claude-Zero/
├── README.md                          This file
├── setup.sh                           Setup script
├── flipper_claude/
│   ├── application.fam                ufbt build manifest
│   └── flipper_claude.c               Flipper Zero app (~700 lines C)
├── mac_bridge/
│   ├── bridge.py                      Python bridge daemon
│   └── com.claude.flipper.plist       launchd template
└── claude_skill/
    ├── SKILL.md                        Claude Code skill instructions
    ├── PROGRESS_SKILL.md               Progress tracking extension
    └── flipper_hook.py                 Python hook library
```

---

## License

MIT
