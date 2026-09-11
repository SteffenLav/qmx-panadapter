#pragma once

#include "lvgl.h"
#include <stdbool.h>

// Full-screen spot-map overlay: a Karte (world map, one dot per spot) and a
// Tabelle (sortable-by-nothing, newest-relevant list) tab over the same live
// spot data POTA/SOTA/RBN/DX-cluster already feed into net/spots.c, plus a
// shared Mode/Quelle filter sidebar that both tabs read. Opened by swiping
// down from the top screen edge (see ui.c's top_edge_swipe_cb) - modelled on
// reader_view.c's overlay pattern (full-screen child of the LVGL screen,
// hidden/foregrounded rather than created and destroyed per open).

// Call once at boot, after the screen exists (mirrors reader_view_init).
void spot_map_view_init(lv_obj_t *parent);

void spot_map_view_show(void);
void spot_map_view_hide(void);

// For ui.c's sync_nav_affordances()/top_bar_apply_mode() - true whenever this
// overlay is the thing on screen, same role as reader_view_is_active().
bool spot_map_view_is_active(void);
