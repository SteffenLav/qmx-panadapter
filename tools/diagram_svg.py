#!/usr/bin/env python3
"""Render a ```qmxdiagram spec to inline SVG for the website.

The device draws the same spec with LVGL widgets (main/ui/reader_diagram.c).
This is the other half: one source, two renderers, so tab5.lav.dk and the Tab5
cannot drift apart the way the old character drawings did - four of which were
found describing behaviour the firmware no longer had, precisely because nobody
could read them well enough on the device to notice.

Deliberately styled as the device screen (dark card, the app's own colours) in
both site themes: these are pictures OF the Tab5, and a diagram of a dark UI
rendered on white stops looking like the thing it describes.

The layout rules mirror the C renderer's, including the one that matters: a
band too narrow for its own label does not get one - the legend carries it
instead. Nothing here is placed by a number a human typed except times.
"""

import re

W = 900                      # one coordinate scale for every diagram, always
PAD = 18
INNER = W - 2 * PAD

COLORS = {
    "steel": "#8AB4F8",
    "amber": "#FFA040",
    "trace": "#4CAF6A",
    "gold":  "#E9C46A",
    "dim":   "#93A4B1",
    "red":   "#E06A5A",
}
SURFACE, RAISED, LINE, INK, DIM = "#151B23", "#1B232D", "#2A3440", "#E6EDF3", "#93A4B1"
FILL_OPA = 0.5               # bands AND legend chips, so they map 1:1

FONT = ("system-ui,-apple-system,'Segoe UI',Roboto,sans-serif")
MONO = "ui-monospace,'Cascadia Mono',Consolas,monospace"


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def color(name):
    return COLORS.get((name or "").strip().lower(), DIM)


def wrap(text, width_px, size):
    """Greedy wrap on an average advance width. Approximate on purpose: the
    boxes below are sized from the RESULT, so a slightly conservative estimate
    costs a little height and never an overlap."""
    per = size * 0.55
    limit = max(8, int(width_px / per))
    words, lines, cur = text.split(), [], ""
    for w in words:
        trial = (cur + " " + w).strip()
        if len(trial) > limit and cur:
            lines.append(cur)
            cur = w
        else:
            cur = trial
    if cur:
        lines.append(cur)
    return lines


def parse(spec):
    """spec text -> (type, [(key, value), ...]) preserving order."""
    kind, items = "", []
    for raw in spec.splitlines():
        line = raw.strip()
        if not line or ":" not in line:
            continue
        k, v = line.split(":", 1)
        k, v = k.strip().lower(), v.strip()
        if k == "type":
            kind = v.lower()
        else:
            items.append((k, v))
    return kind, items


def _text(x, y, s, size=16, fill=INK, anchor="start", family=FONT, weight="normal"):
    return ('<text x="%d" y="%d" font-size="%d" fill="%s" text-anchor="%s" '
            'font-family="%s" font-weight="%s">%s</text>'
            % (x, y, size, fill, anchor, family, weight, esc(s)))


def _notes(items, y):
    """The legend, shared by every diagram type. Chips render at the same fill
    strength as the bands they refer to."""
    out = []
    for k, v in items:
        if k != "note":
            continue
        cname, _, txt = v.partition("|")
        c = color(cname)
        lines = wrap(txt.strip(), INNER - 34, 15)
        out.append('<rect x="%d" y="%d" width="14" height="14" rx="3" fill="%s" '
                   'fill-opacity="%.2f" stroke="%s"/>' % (PAD, y - 11, c, FILL_OPA, c))
        for i, ln in enumerate(lines):
            out.append(_text(PAD + 24, y + i * 19, ln, 15, DIM))
        y += max(22, len(lines) * 19 + 4)
    return out, y


def render(spec):
    kind, items = parse(spec)
    body, y = [], PAD + 8

    title = next((v for k, v in items if k == "title"), None)
    if title:
        body.append(_text(PAD, y + 6, title, 17, COLORS["gold"], weight="600"))
        y += 30

    if kind == "flow":
        first_node = True
        for k, v in items:
            if k == "node":
                # The arrow goes BEFORE each node except the first, matching the
                # C renderer. Emitting it after and trimming the last one was a
                # hack that subtracted its height twice, so the final node sat
                # on top of the legend.
                if not first_node:
                    body.append('<path d="M%d %d l0 14" stroke="%s" stroke-width="1.5"/>'
                                % (PAD + 26, y, DIM))
                    body.append('<path d="M%d %d l-4 -5 l8 0 z" fill="%s"/>'
                                % (PAD + 26, y + 18, DIM))
                    y += 22
                first_node = False
                lines = wrap(v, INNER - 24, 16)
                h = 14 + len(lines) * 21
                body.append('<rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="%s" stroke="%s"/>'
                            % (PAD, y, INNER, h, RAISED, LINE))
                for i, ln in enumerate(lines):
                    body.append(_text(PAD + 12, y + 22 + i * 21, ln, 16, INK))
                y += h + 6
            elif k == "branch":
                lines = wrap(v, INNER - 80, 14)
                h = 10 + len(lines) * 19
                body.append('<rect x="%d" y="%d" width="%d" height="%d" rx="5" fill="%s" stroke="%s"/>'
                            % (PAD + 40, y, INNER - 40, h, SURFACE, LINE))
                body.append('<rect x="%d" y="%d" width="3" height="%d" fill="%s"/>'
                            % (PAD + 40, y, h, COLORS["steel"]))
                for i, ln in enumerate(lines):
                    body.append(_text(PAD + 54, y + 19 + i * 19, ln, 14, DIM))
                y += h + 8

    elif kind == "stack":
        rows = [v for k, v in items if k == "row"]
        total = sum(int(r.split()[0]) for r in rows) or 1
        for r in rows:
            px = int(r.split()[0])
            rest = r.split(None, 1)[1] if len(r.split(None, 1)) > 1 else ""
            name, _, cap = rest.partition("|")
            h = max(30, int(px * 330 / total))
            body.append('<rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="%s" stroke="%s"/>'
                        % (PAD, y, int(INNER * 0.58), h, RAISED, LINE))
            body.append(_text(PAD + 12, y + h // 2 + 6, name.strip(), 16, INK))
            capt = "%d px%s%s" % (px, " - " if cap.strip() else "", cap.strip())
            for i, ln in enumerate(wrap(capt, INNER * 0.40, 15)):
                body.append(_text(PAD + int(INNER * 0.60), y + 20 + i * 19, ln, 15, DIM))
            y += h + 8

    elif kind == "panel":
        for k, v in items:
            if k == "row":
                has_box = v[:1] == "[" and v[2:3] == "]"
                checked = has_box and v[1:2].lower() == "x"
                label = v[3:].strip() if has_box else v
                x = PAD
                if has_box:
                    fill = "#2C5AA0" if checked else SURFACE
                    body.append('<rect x="%d" y="%d" width="22" height="22" rx="4" fill="%s" stroke="%s" stroke-width="2"/>'
                                % (x, y + 6, fill, "#2C5AA0" if checked else LINE))
                    if checked:
                        body.append('<path d="M%d %d l4 5 l8 -10" fill="none" stroke="#fff" '
                                    'stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"/>'
                                    % (x + 5, y + 16))
                    x += 32
                lines = wrap(label, INNER - (x - PAD) - 24, 15)
                h = 12 + len(lines) * 20
                body.append('<rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="%s" stroke="%s"/>'
                            % (x, y, PAD + INNER - x, h, SURFACE, LINE))
                for i, ln in enumerate(lines):
                    body.append(_text(x + 10, y + 21 + i * 20, ln, 15, INK))
                y += h + 8
            elif k == "buttons":
                btns = [b.strip() for b in v.split(",") if b.strip()]
                if not btns:
                    continue
                gap, bw = 10, (INNER - 10 * (len(btns) - 1)) / len(btns)
                for i, b in enumerate(btns):
                    fill = "#2C5AA0"
                    if b.endswith("!"):
                        fill, b = "#962020", b[:-1].strip()
                    elif b.endswith("+"):
                        fill, b = "#2A6B3C", b[:-1].strip()
                    bx = PAD + i * (bw + gap)
                    body.append('<rect x="%.1f" y="%d" width="%.1f" height="38" rx="6" fill="%s"/>'
                                % (bx, y, bw, fill))
                    body.append(_text(bx + bw / 2, y + 25, b, 15, "#ffffff", anchor="middle"))
                y += 46

    elif kind == "timeline":
        span = float(next((v for k, v in items if k == "span"), "1") or 1)
        BAR = 44
        segs = [(k, v) for k, v in items if k in ("seg", "mark")]
        for k, v in segs:
            rng = v.split()[0]
            a, b = (float(x) for x in rng.split("-", 1))
            rest = v.split(None, 1)[1] if len(v.split(None, 1)) > 1 else ""
            cname = rest.split()[0] if rest else "dim"
            label = rest.split(None, 1)[1] if len(rest.split(None, 1)) > 1 else ""
            c = color(cname)
            x0, x1 = PAD + a / span * INNER, PAD + b / span * INNER
            overlay = (k == "mark")
            sy = y + (BAR - 14) if overlay else y
            sh = 14 if overlay else BAR
            body.append('<rect x="%.1f" y="%d" width="%.1f" height="%d" rx="5" fill="%s" '
                        'fill-opacity="%.2f" stroke="%s"/>'
                        % (x0, sy, max(2, x1 - x0), sh, c, FILL_OPA, c))
            # Same rule as the device: too narrow for its own name means no name.
            if not overlay and label and (x1 - x0) > len(label) * 9 + 16:
                body.append(_text((x0 + x1) / 2, y + BAR // 2 + 6, label, 16, INK, anchor="middle"))
        y += BAR + 4
        for k, v in items:
            if k != "tick":
                continue
            for tok in re.split(r"[,\s]+", v.strip()):
                if not tok:
                    continue
                t = float(tok)
                x = PAD + t / span * INNER
                body.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"/>'
                            % (x, y, x, y + 6, LINE))
                anchor = "start" if t <= 0 else ("end" if t >= span else "middle")
                body.append(_text(x, y + 22, tok, 14, DIM, anchor=anchor, family=MONO))
        y += 34

    note_svg, y = _notes(items, y + 10)
    body += note_svg
    h = int(y + PAD)

    return ('<div class="qmx-diagram" style="overflow-x:auto">'
            '<svg viewBox="0 0 %d %d" width="100%%" role="img" '
            'xmlns="http://www.w3.org/2000/svg" style="max-width:%dpx">'
            '<rect x="0" y="0" width="%d" height="%d" rx="10" fill="%s" stroke="%s"/>'
            '%s</svg></div>' % (W, h, W, W, h, SURFACE, LINE, "".join(body)))


FENCE = re.compile(r"```qmxdiagram\n(.*?)```", re.S)


def substitute(markdown):
    """Replace every diagram fence in a page's markdown with its SVG."""
    return FENCE.sub(lambda m: "\n" + render(m.group(1)) + "\n", markdown)
