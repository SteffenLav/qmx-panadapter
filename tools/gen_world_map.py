#!/usr/bin/env python3
"""Regenerate main/util/world_map_data.{c,h} from Natural Earth land polygons.

WHY THIS EXISTS
    The outline Uwe DL8UG contributed is a 1:110m silhouette: 1,280 points for
    the whole world, quantised to 0.1 degrees (~11 km). That is right for a
    thumbnail and wrong once the map zooms. The operator, 2026-09-12: "can the
    map be drawn a bit finer? - especially in zoomed in it looks quite coars",
    and then "far too coarse when zoomed in to Scandinavia for example".

    So the data is regenerated here rather than hand-edited, and this script is
    checked in so the next person can change the tolerance instead of guessing
    what produced the numbers.

WHAT CHANGED IN THE FORMAT, AND WHY EACH PART EARNS ITS BYTES
    - Hundredths of a degree, not tenths. This is FREE: longitude reaches
      +-18000 and latitude +-9000, both inside int16_t. It buys 10x the
      positional resolution for zero extra storage, and at 12x zoom one step
      goes from ~3.5 px of visible stair-stepping to ~0.35 px.
    - A precomputed bounding box per ring. This is NOT decoration. The draw
      loop issues one lv_draw_line per ring EDGE (LVGL 9.2.2 has no polyline
      descriptor), so 16x the points is 16x the draw calls on a board whose
      core 0 is already the wall - and the spot map redrawing is already
      implicated in audio-ring overflows. The bbox lets the renderer reject a
      ring with two projections instead of N, so the zoomed case gets FASTER
      than it is today, and tiny islands fall out on their own when they
      project to less than a pixel. 8 bytes per ring, ~9 KB total.

SOURCE
    Natural Earth 1:50m land polygons, public domain, naturalearthdata.com.
    Fetched from the nvkelso/natural-earth-vector mirror as GeoJSON because
    parsing that needs nothing but the standard library - no shapefile reader,
    no geopandas, neither of which is installed on this machine.

USAGE
    python tools/gen_world_map.py [--tol 0.05] [--min-span 0.15]
"""

import argparse
import json
import math
import os
import sys
import urllib.request

# ⛔ 1:10m, NOT 1:50m. Operator, 2026-09-12: "Map has better resolution,
# however - it is still not good when zooming in [to Scandinavia]." Measured
# rather than re-tuned: at the SAME tolerance (0.05 deg) the 1:50m source gives
# 1,107 rings / 19,543 points; the 1:10m source gives 2,092 rings / 41,375
# points from the SAME simplification pass - i.e. the ceiling was the SOURCE
# dataset's own resolution, not the tolerance chosen on top of it. 1:50m simply
# does not carry a fjord's real shape to begin with; no amount of relaxing
# --tol recovers detail the source never had. 207 KB of rodata against this
# ~267 KB of free flash (confirmed by a real build, not assumed).
SRC_URL = ("https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
           "master/geojson/ne_10m_land.geojson")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT_C = os.path.join(REPO, "main", "util", "world_map_data.c")
OUT_H = os.path.join(REPO, "main", "util", "world_map_data.h")


def fetch(cache):
    if os.path.exists(cache):
        sys.stderr.write("using cached %s\n" % cache)
        return json.load(open(cache, encoding="utf-8"))
    sys.stderr.write("downloading %s\n" % SRC_URL)
    with urllib.request.urlopen(SRC_URL, timeout=120) as r:
        raw = r.read().decode("utf-8")
    open(cache, "w", encoding="utf-8").write(raw)
    return json.loads(raw)


def iter_rings(doc):
    for f in doc["features"]:
        g = f["geometry"]
        polys = [g["coordinates"]] if g["type"] == "Polygon" else g["coordinates"]
        for poly in polys:
            for ring in poly:
                yield ring


def douglas_peucker(pts, tol):
    """Iterative, so a 40,000-point Antarctica cannot blow the Python stack."""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        ax, ay = pts[a]
        bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        den = math.hypot(dx, dy)
        best, idx = 0.0, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            if den == 0.0:
                dist = math.hypot(px - ax, py - ay)
            else:
                dist = abs(dy * px - dx * py + bx * ay - by * ax) / den
            if dist > best:
                best, idx = dist, i
        if idx > 0 and best > tol:
            keep[idx] = True
            stack.append((a, idx))
            stack.append((idx, b))
    return [p for p, k in zip(pts, keep) if k]


def quantise(pts):
    """Degrees -> hundredths, de-duplicated after rounding.

    Rounding can collapse neighbouring points onto each other; leaving the
    duplicates in would cost bytes and emit zero-length line segments."""
    out = []
    for lon, lat in pts:
        x = int(round(lon * 100.0))
        y = int(round(lat * 100.0))
        x = max(-18000, min(18000, x))
        y = max(-9000, min(9000, y))
        if not out or out[-1] != (x, y):
            out.append((x, y))
    # A ring is implicitly closed by the renderer, so an explicit repeat of
    # the first point at the end is a wasted vertex and a zero-length edge.
    while len(out) > 1 and out[0] == out[-1]:
        out.pop()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tol", type=float, default=0.05,
                    help="Douglas-Peucker tolerance in degrees (default 0.05)")
    ap.add_argument("--min-span", type=float, default=0.15,
                    help="drop rings whose bbox is smaller than this, in degrees")
    ap.add_argument("--cache", default=os.path.join(HERE, "ne_10m_land.geojson"))
    args = ap.parse_args()

    doc = fetch(args.cache)

    rings = []
    for raw in iter_rings(doc):
        xs = [p[0] for p in raw]
        ys = [p[1] for p in raw]
        if max(max(xs) - min(xs), max(ys) - min(ys)) < args.min_span:
            continue
        pts = quantise(douglas_peucker([(p[0], p[1]) for p in raw], args.tol))
        if len(pts) < 3:
            continue
        rings.append(pts)

    # Biggest first. Nothing depends on the order, but it makes the generated
    # file readable and puts the continents where a human will look for them.
    rings.sort(key=len, reverse=True)

    total = sum(len(r) for r in rings)
    pt_bytes = total * 4
    bbox_bytes = len(rings) * 8
    tbl_bytes = len(rings) * (4 + 2 + 8)

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER_C % dict(url=SRC_URL, tol=args.tol, span=args.min_span,
                                rings=len(rings), points=total,
                                kb=(pt_bytes + bbox_bytes) / 1024.0))
        for i, r in enumerate(rings):
            flat = ",".join("%d,%d" % (x, y) for x, y in r)
            f.write("static const int16_t ring_%d[] = {%s};\n" % (i, flat))
        f.write("\nconst world_map_ring_t WORLD_MAP_RINGS[] = {\n")
        for i, r in enumerate(rings):
            xs = [p[0] for p in r]
            ys = [p[1] for p in r]
            f.write("    {ring_%d, %d, %d, %d, %d, %d},\n"
                    % (i, len(r), min(xs), min(ys), max(xs), max(ys)))
        f.write("};\nconst int WORLD_MAP_RING_COUNT = %d;\n" % len(rings))

    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER_H % dict(url=SRC_URL, tol=args.tol, span=args.min_span,
                                rings=len(rings), points=total))

    sys.stderr.write(
        "wrote %d rings, %d points\n"
        "  point data %d B, bboxes %d B, table %d B  => %.1f KB of rodata\n"
        % (len(rings), total, pt_bytes, bbox_bytes, tbl_bytes,
           (pt_bytes + bbox_bytes + tbl_bytes) / 1024.0))


HEADER_C = '''// GENERATED by tools/gen_world_map.py - DO NOT EDIT BY HAND.
//
// Source: Natural Earth 1:50m land polygons (public domain,
// naturalearthdata.com), via %(url)s
// Douglas-Peucker tolerance %(tol)g deg, rings smaller than %(span)g deg dropped.
// %(rings)d rings, %(points)d points, %(kb).1f KB.
//
// The module this replaces was contributed by Uwe DL8UG (a 1:110m silhouette
// ported from his rbn_monitor project). Only the DATA is regenerated here -
// his ring/table shape is kept, plus a per-ring bounding box the renderer
// culls with. See the generator for why each part earns its bytes.
#include "world_map_data.h"

'''

HEADER_H = '''// GENERATED by tools/gen_world_map.py - DO NOT EDIT BY HAND.
//
// World land outline for ui/spot_map_view.c.
//
// Originally contributed by Uwe DL8UG, ported from his rbn_monitor project as
// a 1:110m silhouette in tenths of a degree. Regenerated 2026-09-12 at higher
// resolution because the map zooms and the silhouette did not survive it (the
// operator: "far too coarse when zoomed in to Scandinavia").
//
// Source: Natural Earth 1:50m land polygons (public domain), via
// %(url)s
// Douglas-Peucker tolerance %(tol)g deg; rings with a bounding box smaller
// than %(span)g deg are dropped. %(rings)d rings, %(points)d points.
#pragma once

#include <stdint.h>

// ⛔ COORDINATES ARE HUNDREDTHS OF A DEGREE - divide by 100.0, not 10.0.
//
// The previous generation of this table used TENTHS. Hundredths cost nothing
// (longitude reaches +-18000 and latitude +-9000, both inside int16_t) and
// remove the visible stair-stepping when zoomed: at 12x one step is ~0.35 px
// instead of ~3.5 px.
#define WORLD_MAP_UNITS_PER_DEG 100.0f

// Each ring is an implicitly closed polygon outline - the renderer draws the
// edge from the last point back to the first, so do not repeat point 0.
//
// ⭐ THE BOUNDING BOX IS LOAD-BEARING, NOT METADATA. LVGL 9.2.2's
// lv_draw_line_dsc_t is a single segment, so the renderer issues one draw call
// per EDGE. This table is ~16x denser than the silhouette it replaced, and the
// spot map's redraw cost is already implicated in audio-ring overflows on this
// board. Reject a ring by its box (two projections) before walking its points,
// and skip one that projects smaller than a pixel or two - that makes the
// zoomed-in case cheaper than the old coarse table was, and lets the small
// islands cost nothing at low zoom.
typedef struct {
    const int16_t *points;    // interleaved lon,lat pairs, hundredths of a degree
    uint16_t point_count;
    int16_t  lon_min, lat_min, lon_max, lat_max;   // hundredths of a degree
} world_map_ring_t;

extern const world_map_ring_t WORLD_MAP_RINGS[];
extern const int WORLD_MAP_RING_COUNT;
'''


if __name__ == "__main__":
    main()
