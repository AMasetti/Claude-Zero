#!/usr/bin/env bash
# setup.sh — Claude-Zero setup script
# Installs Python deps, configures launchd, and prints Flipper build instructions.
# Usage: bash setup.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRIDGE_DIR="$SCRIPT_DIR/mac_bridge"
BRIDGE_PATH="$BRIDGE_DIR/bridge.py"
PLIST_SRC="$BRIDGE_DIR/com.claude.flipper.plist"
PLIST_DST="$HOME/Library/LaunchAgents/com.claude.flipper.plist"
LOG_PATH="$HOME/.claude_flipper.log"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

info()  { echo "[INFO]  $*"; }
ok()    { echo "[OK]    $*"; }
warn()  { echo "[WARN]  $*"; }
error() { echo "[ERROR] $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Check Python
# ---------------------------------------------------------------------------

info "Checking Python 3..."
PYTHON=$(which python3 2>/dev/null || true)
if [[ -z "$PYTHON" ]]; then
    error "python3 not found. Install Python 3.9+ first."
fi

PY_VERSION=$("$PYTHON" -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
info "Found Python $PY_VERSION at $PYTHON"

# ---------------------------------------------------------------------------
# 2. Install Python dependencies
# ---------------------------------------------------------------------------

info "Installing Python dependencies (pyserial, click)..."
"$PYTHON" -m pip install --quiet --upgrade pyserial click
ok "Python dependencies installed"

# ---------------------------------------------------------------------------
# 3. Install launchd plist (macOS only)
# ---------------------------------------------------------------------------

if [[ "$(uname)" == "Darwin" ]]; then
    info "Configuring launchd agent..."
    mkdir -p "$HOME/Library/LaunchAgents"

    sed \
        -e "s|__PYTHON__|$PYTHON|g" \
        -e "s|__BRIDGE_PATH__|$BRIDGE_PATH|g" \
        -e "s|__LOG_PATH__|$LOG_PATH|g" \
        "$PLIST_SRC" > "$PLIST_DST"

    ok "Plist written to $PLIST_DST"

    # Unload if already loaded (ignore errors)
    launchctl unload "$PLIST_DST" 2>/dev/null || true

    # Load the agent
    if launchctl load "$PLIST_DST" 2>/dev/null; then
        ok "launchd agent loaded — bridge will start on login"
    else
        warn "launchctl load failed (may already be loaded or macOS version differs)"
        warn "Try manually: launchctl load $PLIST_DST"
    fi

    info "Bridge log: $LOG_PATH"
else
    warn "Non-macOS detected. Skipping launchd setup."
    info "To start the bridge manually: python3 $BRIDGE_PATH"
fi

# ---------------------------------------------------------------------------
# 4. Make scripts executable
# ---------------------------------------------------------------------------

chmod +x "$BRIDGE_PATH"
chmod +x "$SCRIPT_DIR/claude_skill/flipper_hook.py"
ok "Scripts made executable"

# ---------------------------------------------------------------------------
# 5. Flipper app build instructions
# ---------------------------------------------------------------------------

cat <<'FLIPPER_INSTRUCTIONS'

╔══════════════════════════════════════════════════════════════════╗
║           Flipper Zero App — Build & Install Instructions        ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                  ║
║  Prerequisites:                                                  ║
║    pip3 install ufbt                                             ║
║                                                                  ║
║  Build the app:                                                  ║
║    cd flipper_claude                                             ║
║    ufbt build                                                    ║
║                                                                  ║
║  Install via USB (Flipper connected):                            ║
║    ufbt launch                                                   ║
║                                                                  ║
║  Or copy manually:                                               ║
║    Copy dist/f7/flipper_claude.fap                               ║
║    to /ext/apps/Tools/ on the Flipper SD card                   ║
║                                                                  ║
║  Then on the Flipper:                                            ║
║    Apps → Tools → Claude Approval                                ║
║                                                                  ║
╚══════════════════════════════════════════════════════════════════╝

FLIPPER_INSTRUCTIONS

# ---------------------------------------------------------------------------
# 6. Claude Code skill setup
# ---------------------------------------------------------------------------

cat <<SKILL_INSTRUCTIONS
╔══════════════════════════════════════════════════════════════════╗
║              Claude Code Skill Configuration                     ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                  ║
║  Add the skill to your Claude Code session by referencing:       ║
║    $SCRIPT_DIR/claude_skill/SKILL.md
║                                                                  ║
║  In your Claude Code project, paste the contents of SKILL.md    ║
║  or reference the skill directory in your settings.             ║
║                                                                  ║
║  For progress tracking, also load:                              ║
║    $SCRIPT_DIR/claude_skill/PROGRESS_SKILL.md
║                                                                  ║
╚══════════════════════════════════════════════════════════════════╝

SKILL_INSTRUCTIONS

# ---------------------------------------------------------------------------
# 7. Quick test
# ---------------------------------------------------------------------------

cat <<'TEST_INSTRUCTIONS'
╔══════════════════════════════════════════════════════════════════╗
║                    Quick Test (No Hardware)                      ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                  ║
║  Terminal 1 — start mock bridge:                                 ║
║    python3 mac_bridge/bridge.py --mock                           ║
║                                                                  ║
║  Terminal 2 — test approval:                                     ║
║    python3 claude_skill/flipper_hook.py approve                  ║
║                                                                  ║
║  The mock bridge auto-approves after 2 seconds.                  ║
║  Set MOCK_AUTO_APPROVE_DELAY=10 to change the delay.             ║
║                                                                  ║
╚══════════════════════════════════════════════════════════════════╝

Setup complete!
TEST_INSTRUCTIONS
