#include "ui_mode.h"

static volatile ui_mode_t s_ui_mode = UI_MODE_PANADAPTER;

ui_mode_t ui_mode_get(void)
{
    return s_ui_mode;
}

void ui_mode_set(ui_mode_t mode)
{
    s_ui_mode = mode;
}
