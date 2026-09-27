#!/usr/bin/env python3
"""Say what color each of the temperatures on a device line was drawn in.

The colors are the point of the field and the plain text capture loses them, so this
keeps the SGR state of the byte stream instead of the screen and reports the sequence
that was in effect over the digits of each temperature. Two fields agree when their
sequences agree.

    tests/temp_colors.py 100 30 -- nvtop
"""

import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_capture import capture  # noqa: E402

SGR = re.compile(r"\x1b\[[0-9;]*m")
OTHER = re.compile(
    r"\x1b\[\??[0-9;]*[a-zA-Z@]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[()][0-9A-B]|\x1b[>=]|\x1b[78Mm]"
)

# The names curses gives the pairs nvtop asks the colors by
NAME = {
    "30": "black",
    "31": "red",
    "32": "green",
    "33": "yellow",
    "34": "blue",
    "35": "magenta",
    "36": "cyan",
    "37": "white",
    "39": "default",
}


def colored_stream(output):
    """The characters the program sent, each with the color selection in force."""
    text = output.decode("utf-8", errors="replace")
    position = 0
    selection = ""
    sent = []
    while position < len(text):
        if text[position] == "\x1b":
            matched = SGR.match(text, position)
            if matched:
                selection = matched.group(0)
                position = matched.end()
                continue
            matched = OTHER.match(text, position)
            if matched:
                position = matched.end()
                continue
        sent.append((text[position], selection))
        position += 1
    return sent


def describe(selection):
    """The color a selection sequence asks for, as a name where it has one."""
    numbers = selection[2:-1].split(";")
    colors = [NAME.get(number, number) for number in numbers if number in NAME]
    bold = any(number == "1" for number in numbers)
    described = "+".join(dict.fromkeys(colors)) or "as before"
    return ("bold " if bold else "") + described


def main():
    columns, rows, argv = int(sys.argv[1]), int(sys.argv[2]), sys.argv[4:]
    if sys.argv[3] != "--":
        raise SystemExit("expected -- before the command")

    sent = colored_stream(capture(columns, rows, argv))
    text = "".join(char for char, _ in sent)

    # The digits of each temperature follow its label; the label carries the color of
    # the whole field, the digits that of the reading itself.
    for field in ("TEMP", "JCT", "VRAM"):
        for found in re.finditer(field + r" *([0-9]+|N/A)", text):
            digits = found.start(1)
            print(
                f"{field:<5} {found.group(1):>5}"
                f"  label: {describe(sent[found.start()][1]):<14}"
                f"  value: {describe(sent[digits][1])}"
            )
        if field not in text:
            print(f"{field}: not drawn")


if __name__ == "__main__":
    main()
