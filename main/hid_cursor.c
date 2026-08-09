// Shared mouse cursor state - see hid_cursor.h.

#include "hid_cursor.h"
#include "esp_log.h"

// The cursor lives in the panel's NATIVE PORTRAIT space (720 x 1280), NOT in
// landscape screen space - because ui.c's indev callback rotates it on the way
// out (screen_x = y, screen_y = (720-1) - x).
//
// Getting this backwards is a real bug that shipped: clamping x to 1280 and y
// to 720 let the rotated result reach y = 1279 on a 720-tall screen, so the
// pointer could sit off the bottom of the display. Movement and clicks still
// LOOKED fine - the cursor is drawn from the same numbers - but any code that
// hit-tests the position (the mouse wheel's scroll target) found nothing
// there, which is exactly how this was finally noticed.
#define PANEL_W 720    // native portrait width  -> becomes screen Y
#define PANEL_H 1280   // native portrait height -> becomes screen X

static volatile int     s_x = PANEL_W / 2;
static volatile int     s_y = PANEL_H / 2;
static volatile uint8_t s_buttons = 0;
static volatile bool    s_present[HID_CURSOR_SRC_COUNT];
static volatile int     s_wheel;   // unconsumed wheel clicks

void hid_cursor_apply(int dx, int dy, uint8_t buttons)
{
    int nx = s_x + dx, ny = s_y + dy;
    if (nx < 0) nx = 0; else if (nx >= PANEL_W) nx = PANEL_W - 1;
    if (ny < 0) ny = 0; else if (ny >= PANEL_H) ny = PANEL_H - 1;
    s_x = nx; s_y = ny;
    s_buttons = buttons;
}

void hid_cursor_add_wheel(int clicks) { s_wheel += clicks; }

int hid_cursor_take_wheel(void)
{
    int v = s_wheel;
    s_wheel = 0;      // consume: a click must scroll exactly once
    return v;
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
