"""External Windows game control. No engine instrumentation or server required."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


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
        if name == "key":
            command.add_argument("keys", help="pywinauto keys, e.g. {SPACE}, n, {F1}")
        if name in ("click", "drag", "scroll"):
            command.add_argument("x", type=int)
            command.add_argument("y", type=int)
        if name in ("click", "drag"):
            command.add_argument("--button", choices=("left", "middle", "right"), default="left")
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

    from pywinauto import Application, mouse
    import win32gui

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
    def point(x, y):
        _, _, width, height = win32gui.GetClientRect(window.handle)
        if not (0 <= x < width and 0 <= y < height):
            raise ValueError("Coordinate lies outside the selected window's client area")
        return win32gui.ClientToScreen(window.handle, (x, y))

    if args.command == "key":
        # GLFW actions consume virtual keys, not VK_PACKET text input.
        window.type_keys(args.keys, pause=0.05, set_foreground=False, vk_packet=False)
    elif args.command == "click":
        mouse.click(button=args.button, coords=point(args.x, args.y))
    elif args.command == "drag":
        start, end = point(args.x, args.y), point(args.to_x, args.to_y)
        mouse.press(button=args.button, coords=start)
        try:
            time.sleep(0.1)
            mouse.move(coords=end)
            time.sleep(0.1)
        finally:
            mouse.release(button=args.button, coords=end)
    elif args.command == "scroll":
        mouse.scroll(coords=point(args.x, args.y), wheel_dist=args.steps)
    elif args.command == "close":
        window.close()
        app.wait_for_process_exit(timeout=30)
        print(json.dumps({"pid": args.pid, "closed": True}))
        return

    if args.capture:
        time.sleep(0.2)
        from PIL import ImageGrab
        _, _, width, height = win32gui.GetClientRect(window.handle)
        left, top = win32gui.ClientToScreen(window.handle, (0, 0))
        args.capture.parent.mkdir(parents=True, exist_ok=True)
        ImageGrab.grab(bbox=(left, top, left + width, top + height),
                       all_screens=True).save(args.capture)
    print(json.dumps({"pid": args.pid, "handle": window.handle, "action": args.command,
                      "capture": str(args.capture) if args.capture else None}))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"game-control: {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(1)
