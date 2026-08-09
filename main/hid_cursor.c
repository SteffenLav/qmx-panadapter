// Shared mouse cursor state - see hid_cursor.h.

#include "hid_cursor.h"
#include "esp_log.h"

// Landscape logical screen (matches the rotated LVGL coordinate space).
#define SCR_W 1280
#define SCR_H 720

static volatile int     s_x = SCR_W / 2;
static volatile int     s_y = SCR_H / 2;
static volatile uint8_t s_buttons = 0;
static volatile bool    s_present[HID_CURSOR_SRC_COUNT];

void hid_cursor_apply(int dx, int dy, uint8_t buttons)
{
    int nx = s_x + dx, ny = s_y + dy;
    if (nx < 0) nx = 0; else if (nx >= SCR_W) nx = SCR_W - 1;
    if (ny < 0) ny = 0; else if (ny >= SCR_H) ny = SCR_H - 1;
    s_x = nx; s_y = ny;
    s_buttons = buttons;
}

void hid_cursor_get(int *x, int *y, uint8_t *buttons)
{
    if (x)       *x = s_x;
    if (y)       *y = s_y;
    if (buttons) *buttons = s_buttons;
}

void hid_cursor_set_present(hid_cursor_src_t src, bool present)
{
    if (src < HID_CURSOR_SRC_COUNT) s_present[src] = present;
}

bool hid_cursor_present(void)
{
    for (int i = 0; i < HID_CURSOR_SRC_COUNT; i++)
        if (s_present[i]) return true;
    return false;
}
