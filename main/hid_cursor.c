// Shared mouse cursor state - see hid_cursor.h.

#include "hid_cursor.h"
#include "esp_log.h"

// The cursor is in LANDSCAPE SCREEN space (1280 x 720).
//
// That is not obvious and I got it wrong once, so: ui.c's indev callback
// applies the INVERSE of LVGL's own ROTATION_90 pointer map, so the two
// cancel and the values here land on screen unchanged. They are screen
// coordinates. Do NOT "correct" this to panel-portrait bounds - doing so
// breaks the cancellation and puts an invisible wall at x = 1280 - 720 = 560,
// which the operator spotted as the cursor refusing to pass the "q" in the
// Freq label.
#define SCR_W 1280
#define SCR_H 720

static volatile int     s_x = SCR_W / 2;
static volatile int     s_y = SCR_H / 2;
static volatile uint8_t s_buttons = 0;
static volatile bool    s_present[HID_CURSOR_SRC_COUNT];
static volatile int     s_wheel;   // unconsumed wheel clicks

void hid_cursor_apply(int dx, int dy, uint8_t buttons)
{
    int nx = s_x + dx, ny = s_y + dy;
    if (nx < 0) nx = 0; else if (nx >= SCR_W) nx = SCR_W - 1;
    if (ny < 0) ny = 0; else if (ny >= SCR_H) ny = SCR_H - 1;
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
