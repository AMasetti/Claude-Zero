# Flipper Zero Approval Gate — Claude Code Skill

## Purpose

This skill intercepts potentially dangerous Claude Code actions and requires
**physical approval** on a connected Flipper Zero device before proceeding.

The Flipper Zero displays a tamagotchi-style character. When Claude is about
to run a dangerous command, the character waves its arms and the action
description scrolls across the screen. The user presses:

- **Right button** → APPROVE (Claude proceeds)
- **Left button** → DENY (Claude aborts)
- **Back button** → CANCEL (Claude aborts)

---

## When to Request Approval

### 1. Bash / Shell Commands

Import and call `gate_bash()` before any bash execution. It intercepts
commands containing:

```
rm, sudo, curl, wget, pip install, npm install, yarn add,
git push, chmod, chown, dd, mkfs, kill, pkill, systemctl, launchctl
```

```python
from claude_skill.flipper_hook import gate_bash

command = "rm -rf /tmp/old_build"
if not gate_bash(command):
    raise PermissionError("Action denied on Flipper Zero")
# proceed with bash execution
```

### 2. File Writes Outside the Working Directory

Use `gate_write()` before writing any file. It intercepts writes to paths
that are **outside the current working directory**.

```python
from claude_skill.flipper_hook import gate_write

if not gate_write("/etc/hosts"):
    raise PermissionError("Action denied on Flipper Zero")
# proceed with file write
```

Writes inside the current working directory pass through without prompting.

### 3. Web Fetch / HTTP Requests

Use `gate_fetch()` before fetching URLs. It passes through whitelisted
domains and intercepts everything else:

**Whitelisted (no prompt):**
- `api.anthropic.com`
- `docs.anthropic.com`
- `raw.githubusercontent.com`
- `pypi.org`
- `registry.npmjs.org`

```python
from claude_skill.flipper_hook import gate_fetch

url = "https://example.com/api/data"
if not gate_fetch(url):
    raise PermissionError("Action denied on Flipper Zero")
# proceed with HTTP request
```

### 4. Any Destructive or Sensitive Tool

For tools explicitly marked destructive or sensitive, call
`request_approval()` directly:

```python
from claude_skill.flipper_hook import request_approval

if not request_approval("destructive", "Deploy to production database"):
    raise PermissionError("Action denied on Flipper Zero")
```

---

## Approval Flow

```
Claude wants to run: rm -rf /tmp/old_build
         │
         ▼
  flipper_hook.gate_bash("rm -rf /tmp/old_build")
         │
         ▼
  should_intercept_bash() → True  (contains "rm")
         │
         ▼
  request_approval("bash", "bash: rm -rf /tmp/old_build")
         │
         ▼
  Sends JSON to bridge → /tmp/flipper_claude.sock
         │
         ▼
  Bridge forwards: NOTIFY:ALERT:bash: rm -rf /tmp/old_build
         │
         ▼
  Flipper shows alert animation + scrolling command
         │
         ├── User presses Right (OK)  → bridge returns {"approved": true}
         │                              gate_bash() returns True → proceed
         │
         └── User presses Left (DENY) → bridge returns {"approved": false}
                                         gate_bash() returns False → abort
```

---

## Failure Modes

| Situation | Behavior |
|-----------|----------|
| Bridge not running (Flipper disconnected) | **Fail open** — proceed without approval |
| 30-second timeout (user walks away) | **Fail closed** — deny action |
| User presses DENY | Deny action |
| User presses CANCEL / Back | Deny action |
| Bridge returns unknown error | Deny action |

**Fail-open rationale:** The Flipper Zero is an *optional enhancement*, not a
hard dependency. Claude must remain functional when the Flipper is not
connected. Fail-open means Claude works normally without it.

---

## State Notifications

Send non-blocking state hints to the Flipper to keep the character animated:

```python
from claude_skill.flipper_hook import notify_state

notify_state("think")   # Claude is processing (thought bubble + ZZZ)
notify_state("wait")    # Claude is waiting for external event (spinner)
notify_state("idle")    # Claude is idle (walking character)
```

These are fire-and-forget — errors are silently ignored.

---

## Setup

See [README.md](../README.md) for full setup instructions.

Quick summary:
1. Run `bash setup.sh` to install deps and configure launchd
2. Build and install the Flipper app with `ufbt` (see README)
3. Start the bridge: `python3 mac_bridge/bridge.py` (or let launchd manage it)
4. Point your Claude Code configuration to `claude_skill/`

---

## Testing Without Hardware

```bash
# Start bridge in mock mode (auto-approves after 2s)
python3 mac_bridge/bridge.py --mock

# Run the hook self-test
python3 claude_skill/flipper_hook.py approve

# Interactive mock (type OK/DENY/CANCEL manually)
python3 mac_bridge/bridge.py --mock --interactive
```

---

## API Reference

```python
from claude_skill.flipper_hook import (
    request_approval,   # request_approval(kind, message) -> bool
    notify_state,       # notify_state("idle"|"think"|"wait") -> None
    report_progress,    # report_progress(0-100, message) -> None
    gate_bash,          # gate_bash(command) -> bool
    gate_write,         # gate_write(path, cwd=None) -> bool
    gate_fetch,         # gate_fetch(url) -> bool
    should_intercept_bash,   # predicate only, no approval
    should_intercept_write,  # predicate only
    should_intercept_fetch,  # predicate only
)
```
