/* Host stub for main/util/panic_hook.h.
 *
 * adif_log.c marks its SPIFFS gc/check pair with the T138 breadcrumb
 * (panic_hook_crumb_enter/exit). Those write RTC no-init RAM, which does not
 * exist on the host - and the harness is testing the worked-call cache, not
 * the breadcrumb. -Itest/shim comes first in CFLAGS, so this header is found
 * instead of the firmware one and the real adif_log.c still compiles
 * unmodified, which is the whole point of this shim directory.
 *
 * ⚠ The breadcrumb's OWN correctness is checked by tools/check_crumbs.py, not
 * here. If that ever changes, this stub has to grow with it rather than be
 * quietly deleted. */
#pragma once
#include <stdbool.h>
static inline void panic_hook_crumb_enter(const char *site) { (void)site; }
static inline void panic_hook_crumb_exit(void) { }
