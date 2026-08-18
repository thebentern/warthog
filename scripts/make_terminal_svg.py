#!/usr/bin/env python3
"""Render captured terminal output as a self-contained SVG.

The screenshots in the README are generated from REAL sessions against real
hardware rather than mocked up, so they cannot quietly drift into showing
output the firmware no longer produces. Regenerate with
scripts/regen-screenshots.sh after a change that alters what these commands
print.

SVG rather than PNG: crisp at any width, a few kB, diffs as text, and GitHub
renders it inline. The card paints its own dark background so it looks the same
in either GitHub theme.

Usage: make_terminal_svg.py <capture.txt> <out.svg> "Window title"
"""
import html
import sys

BG, BAR, FG = "#11151c", "#1b212b", "#c8d3e0"
DIM, PROMPT, CMD = "#5c6b7f", "#7aa2f7", "#e6edf5"
OK, ERR, VAL, KEY = "#7ee787", "#ff7b72", "#79c0ff", "#d2a8ff"

CH_W, LINE_H, PAD, BAR_H = 8.4, 20.0, 18.0, 34.0
FONT = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace"


def classify(s):
    """(text, colour) runs for one line. Matches on the line itself -- the
    captures are plain text, and these are the only shapes warthog emits."""
    for sep in ("$ ", "warthog> "):
        if s.startswith(sep):
            return [(sep, PROMPT), (s[len(sep):], CMD)]
    if s.startswith("+ERR") or s.startswith("ERROR"):
        return [(s, ERR)]
    if s.strip() == "OK":
        return [(s, OK)]
    if s.startswith("+"):
        head, _, rest = s.partition(":")
        return [(head + ":", KEY)] + ([(rest, VAL)] if rest else [])
    if "0.0% packet loss" in s or "0% packet loss" in s:
        return [(s, OK)]
    if s.startswith("---") or s.startswith("PING"):
        return [(s, DIM)]
    return [(s, FG)]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    src, dst, title = sys.argv[1:4]
    lines = open(src).read().rstrip("\n").split("\n")

    cols = max([len(l) for l in lines] + [len(title) + 14])
    w = int(PAD * 2 + cols * CH_W)
    h = int(BAR_H + PAD + len(lines) * LINE_H + PAD)

    o = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" role="img" aria-label="{html.escape(title)}">',
        f'<rect width="{w}" height="{h}" rx="10" fill="{BG}"/>',
        f'<path d="M0 10a10 10 0 0 1 10-10h{w - 20}a10 10 0 0 1 10 10v{BAR_H - 10}H0z" fill="{BAR}"/>',
    ]
    for i, c in enumerate(("#ff5f57", "#febc2e", "#28c840")):
        o.append(f'<circle cx="{20 + i * 18}" cy="{BAR_H / 2}" r="5.5" fill="{c}"/>')
    o.append(
        f'<text x="{w / 2}" y="{BAR_H / 2 + 4}" fill="{DIM}" text-anchor="middle" '
        f'font-family="{FONT}" font-size="12">{html.escape(title)}</text>'
    )

    y = BAR_H + PAD + 12
    for line in lines:
        x = PAD
        for text, colour in classify(line):
            if text:
                o.append(
                    f'<text x="{x:.1f}" y="{y:.1f}" fill="{colour}" xml:space="preserve" '
                    f'font-family="{FONT}" font-size="13">{html.escape(text)}</text>'
                )
                x += len(text) * CH_W
        y += LINE_H
    o.append("</svg>")
    open(dst, "w").write("\n".join(o) + "\n")
    print(f"  {dst}  {w}x{h}  ({len(lines)} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
