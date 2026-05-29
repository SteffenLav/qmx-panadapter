#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Battery state of charge, 0-100. Returns -1 if unknown / unsupported.
// Stub for now; real INA226 wiring to follow.
int battery_get_level(void);

// True when current is flowing into the battery (charging).
bool battery_is_charging(void);

#ifdef __cplusplus
}
#endif