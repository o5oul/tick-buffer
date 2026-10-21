#!/usr/bin/env python3
"""
Tick Buffer terminal controller (o5oul)

Press the hotkey (default F7, works while Geometry Dash is focused) to cycle:
    OFF -> +2 ticks -> +10 ticks -> OFF -> ...

It writes "early late" to a small file that the Tick Buffer Geode mod polls.
While this program is running it overrides the mod's sliders; when you quit it
(Ctrl+C), the file is removed and the mod goes back to its own settings.

Usage:
    pip install pynput
    python tickbuffer_ctl.py
    python tickbuffer_ctl.py --key f8 --modes 2,10,20 --sides both
"""

import argparse
import atexit
import os
import sys
import time
from pathlib import Path

try:
    from pynput import keyboard
except ImportError:
    sys.exit("Missing dependency. Run:  pip install pynput")


def mode_file() -> Path:
    # Must match extPath() in the mod.
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", "."))
        return base / "TickBuffer" / "mode.txt"
    return Path.home() / ".tickbuffer" / "mode.txt"


def parse_key(name: str):
    name = name.lower()
    if hasattr(keyboard.Key, name):
        return getattr(keyboard.Key, name)
    if len(name) == 1:
        return keyboard.KeyCode.from_char(name)
    sys.exit(f"Unknown key '{name}'. Try f6..f12, or a single character.")


class Controller:
    def __init__(self, path: Path, extras: list[int], sides: str):
        self.path = path
        # index 0 is always OFF
        self.modes = [0] + extras
        self.sides = sides
        self.index = 0
        path.parent.mkdir(parents=True, exist_ok=True)
        atexit.register(self.release)
        self.apply()

    def values(self):
        n = self.modes[self.index]
        early = n if self.sides in ("both", "early") else 0
        late = n if self.sides in ("both", "late") else 0
        return early, late

    def apply(self):
        early, late = self.values()
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(f"{early} {late}\n")
        tmp.replace(self.path)  # atomic, so the mod never reads a half-written file
        self.render()

    def next_mode(self):
        self.index = (self.index + 1) % len(self.modes)
        self.apply()

    def release(self):
        try:
            self.path.unlink(missing_ok=True)
        except OSError:
            pass

    def render(self):
        n = self.modes[self.index]
        early, late = self.values()
        if n == 0:
            label = "\033[91mOFF\033[0m"
            detail = "no extra ticks"
        else:
            label = f"\033[92m+{n} TICKS\033[0m"
            total = early + late + 1
            detail = f"early {early} / late {late}  (window {total} ticks, ~{total * 1000 / 240:.1f}ms @240 TPS)"
        sys.stdout.write(f"\r\033[K  Mode: {label}   {detail}")
        sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser(description="Tick Buffer terminal controller")
    ap.add_argument("--key", default="f7", help="hotkey that cycles modes (default f7)")
    ap.add_argument("--modes", default="2,10", help="comma-separated extra-tick modes (default 2,10)")
    ap.add_argument("--sides", choices=["both", "early", "late"], default="both",
                    help="apply the extra ticks before the click, after it, or both (default both)")
    ap.add_argument("--file", help="override the mode file path")
    args = ap.parse_args()

    try:
        extras = [max(0, min(240, int(x))) for x in args.modes.split(",") if x.strip()]
    except ValueError:
        sys.exit("--modes must be comma-separated integers, e.g. 2,10")
    if not extras:
        sys.exit("Give at least one mode, e.g. --modes 2,10")

    if os.name == "nt":
        os.system("")  # enable ANSI colours in the Windows console

    ctl = Controller(Path(args.file) if args.file else mode_file(), extras, args.sides)
    hotkey = parse_key(args.key)

    print("\033[1mTick Buffer controller\033[0m")
    print(f"  Hotkey : {args.key.upper()} (global)   Modes: OFF -> " +
          " -> ".join(f"+{n}" for n in extras) + " -> OFF")
    print(f"  File   : {ctl.path}")
    print("  Ctrl+C to quit (releases control back to the mod's own settings)\n")
    ctl.render()

    def on_press(key):
        if key == hotkey:
            ctl.next_mode()

    with keyboard.Listener(on_press=on_press) as listener:
        try:
            while listener.running:
                time.sleep(0.2)
        except KeyboardInterrupt:
            pass
    print("\n  Released control. Bye!")


if __name__ == "__main__":
    main()
