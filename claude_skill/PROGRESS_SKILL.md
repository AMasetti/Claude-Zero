# Flipper Zero Progress Tracking — Extension Skill

## Purpose

This extension skill teaches Claude Code to send **real-time progress updates**
to the Flipper Zero during long-running tasks. The Flipper displays a progress
bar and status text in its WAITING animation state, giving you a physical
indicator of what Claude is doing without needing to watch the terminal.

---

## Extension Prompt

Paste the following prompt into a Claude Code session after the base skill
(`SKILL.md`) is active:

---

```
Extend the Flipper Zero skill to report progress on long tasks.

Rules:
1. For any task that involves more than ~3 sequential steps, or that will take
   more than a few seconds, use report_progress() to send updates to the Flipper.

2. Call report_progress() at these moments:
   - Task begin:     report_progress(0, "Starting: <brief task name>")
   - Each milestone: report_progress(N, "<current step description>")
   - Task complete:  report_progress(100, "Done: <task name>")

3. Use evenly-spaced percentages. For a 5-step task: 0, 20, 40, 60, 80, 100.

4. Keep messages short (≤40 chars) — the Flipper screen is small.

5. Always pair with notify_state("wait") before the task and
   notify_state("idle") after it completes.

Example pattern:
    from claude_skill.flipper_hook import notify_state, report_progress

    notify_state("wait")
    report_progress(0, "Installing deps...")

    # step 1
    report_progress(25, "npm install")
    run_npm_install()

    # step 2
    report_progress(50, "Building...")
    run_build()

    # step 3
    report_progress(75, "Running tests...")
    run_tests()

    report_progress(100, "Done!")
    notify_state("idle")

Apply this pattern to all multi-step tasks from now on.
```

---

## Protocol Extension

The `report_progress()` function sends this serial command to the Flipper:

```
PROGRESS:<0-100>:<message>\n
```

Examples:
```
PROGRESS:0:Starting build...\n
PROGRESS:33:Compiling sources\n
PROGRESS:66:Linking\n
PROGRESS:100:Build complete\n
```

The bridge passes this through immediately with no approval wait:
```json
→ bridge:  {"action": "PROGRESS", "percent": 33, "message": "Compiling"}
← bridge:  {"ok": true}
```

The Flipper app renders a filled progress bar at the bottom of the WAITING
state screen, with the message text above it.

---

## API

```python
from claude_skill.flipper_hook import report_progress, notify_state

# Send a progress update (fire-and-forget, never raises)
report_progress(percent: int, message: str = "") -> None

# Send state hint (fire-and-forget)
notify_state("wait")   # show spinner (before long task)
notify_state("idle")   # return to walking character (after task)
```

---

## Flipper Display

During a long task, the Flipper screen shows:

```
+--------------------------------+
|                                |
|   [spinning arc animation]     |
|                                |
|  Compiling sources...          |
| [################      ] 66%   |
+--------------------------------+
```

The progress bar fills left-to-right proportional to the percent value.
The message text appears above the bar.

---

## Example: Multi-file Refactor

```python
from claude_skill.flipper_hook import notify_state, report_progress

files = get_files_to_refactor()
total = len(files)

notify_state("wait")
report_progress(0, f"Refactoring {total} files")

for i, f in enumerate(files):
    pct = int((i / total) * 100)
    report_progress(pct, f"Editing {os.path.basename(f)}")
    refactor_file(f)

report_progress(100, "Refactor complete")
notify_state("idle")
```

---

## Notes

- `report_progress()` is **non-blocking** and **best-effort** — if the bridge
  is down, it silently does nothing. Never let it block Claude's work.
- Progress updates do NOT require Flipper approval — they are one-way cosmetic.
- There is no strict requirement on update frequency. Avoid calling it more
  than once per second to keep the display readable.
- If a task fails mid-way, call `report_progress(0, "Failed")` then
  `notify_state("idle")` to reset the Flipper state.
