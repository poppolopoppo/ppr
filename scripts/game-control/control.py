"""External Windows game control. No engine instrumentation or server required."""

import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import time

# ------------------------------------------------------------------
# input timing and event constants
# ------------------------------------------------------------------

BUTTON_FLAGS = {
    "left": (0x0002, 0x0004),
    "right": (0x0008, 0x0010),
    "middle": (0x0020, 0x0040),
}
MOUSEEVENTF_WHEEL = 0x0800
WHEEL_DELTA = 120
DRAG_STEPS = 8

# The game samples input once per frame, so a button must stay down across at
# least one frame, and a capture must wait for a frame before observing the result.
HOLD_SECONDS = 0.12
SETTLE_SECONDS = 0.5


def enable_dpi_awareness():
    """Give window metrics, cursor placement, and captures one coordinate space."""
    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        return
    except (AttributeError, OSError):
        pass

    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except (AttributeError, OSError):
        ctypes.windll.user32.SetProcessDPIAware()


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    launch = commands.add_parser("launch")
    launch.add_argument("executable", type=Path)
    launch.add_argument("--log", type=Path, required=True)
    launch.add_argument("arguments", nargs=argparse.REMAINDER)
    for name in ("windows", "capture", "key", "click", "drag", "scroll", "close"):
        command = commands.add_parser(name)
        command.add_argument("--pid", type=int, required=True)
        command.add_argument("--handle", type=lambda value: int(value, 0))
        if name not in ("windows", "close"):
            command.add_argument("--capture", type=Path, required=name == "capture")
        if name in ("capture", "key", "click", "drag", "scroll"):
            command.add_argument("--settle", type=float, default=SETTLE_SECONDS,
                                 help="Seconds to wait before a post-action capture")
        if name == "key":
            command.add_argument("keys", help="pywinauto keys, e.g. {SPACE}, n, {F1}")
        if name in ("click", "drag", "scroll"):
            command.add_argument("x", type=int)
            command.add_argument("y", type=int)
        if name in ("click", "drag"):
            command.add_argument("--button", choices=("left", "middle", "right"), default="left")
            command.add_argument("--hold", type=float, default=HOLD_SECONDS,
                                 help="Seconds to keep the button down; one frame must elapse")
        if name == "drag":
            command.add_argument("to_x", type=int)
            command.add_argument("to_y", type=int)
        if name == "scroll":
            command.add_argument("steps", type=int, help="Wheel detents; positive is up")
    return result


def main():
    args = parser().parse_args()
    if sys.platform != "win32":
        raise RuntimeError("This controller requires Windows and an unlocked interactive desktop")
    enable_dpi_awareness()
    if args.command == "launch":
        executable = args.executable.resolve(strict=True)
        args.log.parent.mkdir(parents=True, exist_ok=True)
        arguments = args.arguments
        if arguments[:1] == ["--"]:
            arguments = arguments[1:]
        with args.log.open("ab", buffering=0) as output:
            process = subprocess.Popen([str(executable), *arguments], cwd=executable.parent,
                                       stdout=output, stderr=subprocess.STDOUT)
        print(json.dumps({"pid": process.pid, "executable": str(executable), "log": str(args.log)}))
        return

    from pywinauto import Application
    import win32api
    import win32gui

    user32 = ctypes.windll.user32

    app = Application(backend="win32").connect(process=args.pid, timeout=10)
    windows = [window for window in app.windows() if window.is_visible()]
    if args.command == "windows":
        print(json.dumps([{"handle": window.handle, "title": window.window_text()}
                          for window in windows]))
        return
    candidates = [window for window in windows if args.handle is None or window.handle == args.handle]
    if len(candidates) != 1:
        raise RuntimeError("Expected one visible window; use windows and --handle to select explicitly")
    window = candidates[0]
    window.set_focus()
    if win32gui.GetForegroundWindow() != window.handle:
        raise RuntimeError("Could not focus the selected window; no input sent")

    # Coordinates and captures both use the client area, excluding title bar/borders.
    def client_size():
        _, _, width, height = win32gui.GetClientRect(window.handle)
        return width, height

    def client_point(x, y):
        width, height = client_size()
        if not (0 <= x < width and 0 <= y < height):
            raise ValueError(f"Coordinate ({x}, {y}) lies outside the selected window's client area of "
                             f"{width}x{height}")
        return win32gui.ClientToScreen(window.handle, (x, y))

    def move_cursor(screen):
        user32.SetCursorPos(*screen)
        landed = win32api.GetCursorPos()
        if landed != tuple(screen):
            raise RuntimeError(f"Cursor landed at {landed} instead of the requested screen point {tuple(screen)}")

    def set_button(button, down):
        user32.mouse_event(BUTTON_FLAGS[button][0 if down else 1], 0, 0, 0, 0)

    if args.command == "key":
        # GLFW actions consume virtual keys, not VK_PACKET text input.
        window.type_keys(args.keys, pause=HOLD_SECONDS, set_foreground=False, vk_packet=False)
    elif args.command == "click":
        screen_point = client_point(args.x, args.y)
        move_cursor(screen_point)
        set_button(args.button, True)
        time.sleep(args.hold)
        set_button(args.button, False)
    elif args.command == "drag":
        start, end = client_point(args.x, args.y), client_point(args.to_x, args.to_y)
        move_cursor(start)
        set_button(args.button, True)
        try:
            time.sleep(args.hold)
            for step in range(1, DRAG_STEPS + 1):
                fraction = step / DRAG_STEPS
                move_cursor((round(start[0] + (end[0] - start[0]) * fraction),
                             round(start[1] + (end[1] - start[1]) * fraction)))
                time.sleep(args.hold)
        finally:
            set_button(args.button, False)
    elif args.command == "scroll":
        move_cursor(client_point(args.x, args.y))
        user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, WHEEL_DELTA * args.steps, 0)
    elif args.command == "close":
        window.close()
        app.wait_for_process_exit(timeout=30)
        print(json.dumps({"pid": args.pid, "closed": True}))
        return

    if args.capture:
        time.sleep(args.settle)
        from PIL import ImageGrab
        width, height = client_size()
        left, top = win32gui.ClientToScreen(window.handle, (0, 0))
        args.capture.parent.mkdir(parents=True, exist_ok=True)
        ImageGrab.grab(bbox=(left, top, left + width, top + height),
                       all_screens=True).save(args.capture)
    width, height = client_size()
    print(json.dumps({"pid": args.pid, "handle": window.handle, "action": args.command,
                      "client_size": [width, height],
                      "screen": list(win32api.GetCursorPos())
                      if args.command in ("click", "drag", "scroll") else None,
                      "capture": str(args.capture) if args.capture else None}))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"game-control: {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(1)
