---
name: game-control
description: Launch, observe, and interact with the Windows game process using external screenshots, keyboard, mouse, and wheel input. Use for interactive game debugging rather than engine-state queries.
---

# Game control

Use the installed Computer Use plugin when available; read its skill and use its
window-targeted APIs. The repository also provides a terminal-driven controller
at `scripts/game-control/control.py`, based on pywinauto and Pillow. It requires
Windows and an unlocked interactive desktop. It adds no engine code or runtime
dependency to the shipped game. Do not operate through a locked desktop.

## Terminal controller

Create a local environment once (the build shell is only needed to build):

```powershell
python -m venv .slim/tmp/game-control-venv
.slim/tmp/game-control-venv/Scripts/python.exe -m pip install -r scripts/game-control/requirements.txt
```

Use `control.py --help` and `<command> --help` for arguments. Each invocation
prints JSON on success and exits nonzero with a diagnostic on failure. In the
examples below, replace `25892` with the PID returned by launch:

```powershell
.slim/tmp/game-control-venv/Scripts/python.exe scripts/game-control/control.py launch --log .slim/tmp/game-run.log out/build/msvc-rel/game/app.game.exe
.slim/tmp/game-control-venv/Scripts/python.exe scripts/game-control/control.py windows --pid 25892
.slim/tmp/game-control-venv/Scripts/python.exe scripts/game-control/control.py capture --pid 25892 --capture .slim/tmp/game.png
.slim/tmp/game-control-venv/Scripts/python.exe scripts/game-control/control.py key --pid 25892 '{SPACE}' --capture .slim/tmp/paused.png
.slim/tmp/game-control-venv/Scripts/python.exe scripts/game-control/control.py close --pid 25892
```

Launch options precede the executable; arguments after it go to the game (an
optional `--` separator is accepted). Prefer Release for interaction; ASan builds
need the activated toolchain runtime PATH. Launch returns before startup finishes;
query `windows` again if it returns an empty list during startup. Inspect the log
if the process exits or never creates a window. Do not blindly relaunch.

Input and capture require exactly one visible window for the PID; if there are
several, select a returned handle with `--handle`. The controller focuses the
selected window and refuses input when focus cannot be established. Run it in
the same desktop/security context as the game; sandboxed processes may not see
the window. It can attach to an already running process, too.

Inspect each screenshot before choosing the next action. Coordinates are pixels
relative to the captured **client area**, excluding title bar/borders. Reobserve
after resizing, changing DPI, opening a popup, or moving panels. Screenshots use
the visible desktop: keep the target unobscured and avoid simultaneous user input.

- `click --pid PID X Y [--button left|middle|right]`
- `drag --pid PID X Y TO_X TO_Y [--button left|middle|right]`
- `scroll --pid PID X Y STEPS` (positive wheel detents scroll up)
- `key --pid PID KEYS` uses [pywinauto key syntax](https://pywinauto.readthedocs.io/en/latest/code/pywinauto.keyboard.html), e.g. `{SPACE}`, `n`, `{F1}`. Do not send unmatched key-down sequences.

These input commands accept `--capture PATH` for an observation after the action.
Input is held for at least one frame: `click` and `drag` keep the button down for
`--hold` (default 0.12 s), and `--settle` (default 0.5 s) delays a post-action
capture so a slow frame has rendered before the screenshot is taken. The JSON
result reports the resolved `screen` coordinates and the window `client_size`, so
a click that lands elsewhere is visible instead of silent.
Use Space to pause/resume, N to step while paused, and F1 for the game's current
shortcut list. Prefer observed UI labels over maintaining a duplicate key map.
Close posts the normal window-close request and waits for process exit; it does
not force-kill. A timeout is a shutdown failure to investigate.

## Boundary with the future engine MCP

This is generic OS input and visual observation, usable with any Windows game
window. It cannot inspect simulation state, select semantic ImGui items, or
provide deterministic tick acknowledgements. Keep future semantic MCP commands
inside the engine's owning-thread/lifecycle boundaries; do not restore CSV feeds,
scripted frame behaviours, or telemetry solely for agent interaction.
