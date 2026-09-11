#pragma once

#include <stdint.h>

// Simplified world land outline, derived from Natural Earth 110m land
// polygons (public domain, naturalearthdata.com), decimated with
// Douglas-Peucker (tolerance ~0.6 degrees) and with small islands/features
// dropped -- this is a recognizable low-detail silhouette for a small
// embedded display, not survey-accurate coastline data.
//
// Ported verbatim (data + shape) from the sibling rbn_monitor project's
// world_map_data.h/.cpp -- same board family, same problem, no reason to
// regenerate it here.
//
// Coordinates are tenths of a degree (lon*10, lat*10) so they fit in
// int16_t; divide by 10.0 to get real degrees. Each ring is an implicitly
// closed polygon outline (last point connects back to the first).

typedef struct {
    const int16_t *points; // interleaved lon,lat pairs, tenths of a degree
    uint16_t point_count;
} world_map_ring_t;

extern const world_map_ring_t WORLD_MAP_RINGS[];
extern const int WORLD_MAP_RING_COUNT;
