#!/usr/bin/env python3
"""Run a terminal program in a pseudo terminal of a chosen size and print what it
drew, with the drawing commands taken out. Lets the layout of nvtop be looked at
from a shell that has no terminal of its own.

The size is set before the program starts and the first drawing is what is kept, so
the result is a whole frame rather than the partial updates of the ones after it.

    tests/pty_capture.py 100 44 -- nvtop
    PTY_CAPTURE_HOLD=0.6 tests/pty_capture.py 120 40 -- nvtop -d 10
"""

import fcntl
import os
import pty
import re
import select
import struct
import subprocess
import sys
import termios
import time

# The characters the DEC special graphics set draws where nvtop asks for an ACS one.
DEC_SPECIAL = {
    "j": "┘",
    "k": "┌",
    "l": "┐",
    "m": "├",
    "q": "─",
    "t": "├",
    "u": "┤",
    "v": "┴",
    "w": "┬",
    "x": "│",
    "n": "┼",
    "f": "°",
    "a": "▒",
    "g": "▋",
    "0": "▌",
    "{": "±",
    "}": "≤",
    "~": "·",
}


def capture(columns, rows, argv, hold=1.0, keys=b"", key_delay=0.12):
    master, slave = pty.openpty()
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))

    environment = dict(os.environ, TERM="xterm-256color")
    process = subprocess.Popen(
        argv, stdin=slave, stdout=slave, stderr=slave, close_fds=True, env=environment, start_new_session=True
    )
    os.close(slave)

    # Keys may be a whole string or a list of them, one press per entry, paced so
    # the program sees distinct key presses
    pending = [(time.time() + hold / 3, chunk) for chunk in (keys if isinstance(keys, list) else [keys]) if chunk]
    for position, _ in enumerate(pending[1:], start=1):
        pending[position] = (pending[position - 1][0] + key_delay, pending[position][1])

    captured = bytearray()
    deadline = time.time() + hold
    while time.time() < deadline:
        while pending and pending[0][0] < time.time():
            os.write(master, pending.pop(0)[1])
        readable, _, _ = select.select([master], [], [], 0.05)
        if not readable:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError:
            break
        if not chunk:
            break
        captured += chunk

    if process.poll() is None:
        process.kill()
    process.wait()
    os.close(master)
    return bytes(captured)


# Keeps the text and moves the cursor where the program meant it to, so the result
# reads like the screen did.
def as_screen(output, columns, rows):
    screen = [[" "] * columns for _ in range(rows)]
    row = column = 0
    text = output.decode("utf-8", errors="replace")

    cursor_position = re.compile(r"\x1b\[(\d*)(?:;(\d*))?H")
    cursor_relative = re.compile(r"\x1b\[(\d*)([ABCD])")
    line_absolute = re.compile(r"\x1b\[(\d*)d")
    column_absolute = re.compile(r"\x1b\[(\d*)g")
    charset_designation = re.compile(r"\x1b\((.)")
    erase_display = re.compile(r"\x1b\[(?:2|3)J")
    erase_line = re.compile(r"\x1b\[K")
    other_escape = re.compile(
        r"\x1b\[\??[0-9;]*[a-zA-Z@]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[()][0-9A-B]|\x1b[>=]|\x1b[78Mm]"
    )

    index = 0
    special_graphics = False
    while index < len(text):
        if text[index] == "\x1b":
            for pattern in (
                cursor_position,
                cursor_relative,
                line_absolute,
                column_absolute,
                charset_designation,
                erase_display,
                erase_line,
                other_escape,
            ):
                matched = pattern.match(text, index)
                if not matched:
                    continue
                if pattern is cursor_position:
                    row = int(matched.group(1) or 1) - 1
                    column = int(matched.group(2) or 1) - 1
                elif pattern is cursor_relative:
                    step = int(matched.group(1) or 1)
                    direction = matched.group(2)
                    if direction == "A":
                        row -= step
                    elif direction == "B":
                        row += step
                    elif direction == "C":
                        column += step
                    else:
                        column -= step
                elif pattern is line_absolute:
                    row = int(matched.group(1) or 1) - 1
                    column = 0
                elif pattern is column_absolute:
                    column = int(matched.group(1) or 1) - 1
                elif pattern is charset_designation:
                    special_graphics = matched.group(1) == "0"
                elif pattern is erase_display:
                    screen = [[" "] * columns for _ in range(rows)]
                elif pattern is erase_line:
                    for at in range(column, columns):
                        screen[row][at] = " "
                index = matched.end()
                break
            else:
                index += 1
            continue

        character = text[index]
        index += 1
        if character == "\n":
            row += 1
            column = 0
        elif character == "\r":
            column = 0
        elif character == "\t":
            column = (column // 8 + 1) * 8
        elif character < " ":
            continue
        else:
            if 0 <= row < rows and column < columns:
                if special_graphics and character in DEC_SPECIAL:
                    character = DEC_SPECIAL[character]
                screen[row][column] = character
            column += 1

    return "\n".join("".join(line).rstrip() for line in screen)


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2

    columns, rows = int(sys.argv[1]), int(sys.argv[2])
    argv = sys.argv[3:]
    if argv and argv[0] == "--":
        argv = argv[1:]

    # Keys to press once the program has drawn, for looking at a screen that is not
    # the first one: PTY_CAPTURE_KEYS=$'\x1bOP' for F2, down is $'\x1b[B'
    keys = os.environ.get("PTY_CAPTURE_KEYS", "").encode()

    print(
        as_screen(
            capture(columns, rows, argv, float(os.environ.get("PTY_CAPTURE_HOLD", "1.0")), keys),
            columns,
            rows,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
