/* Tab5 simulator shim: Emscripten's musl has no <sys/cdefs.h>; ESP-IDF headers
 * written for newlib include it for these macros only. */
#pragma once
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif
#ifndef __unused
#define __unused __attribute__((__unused__))
#endif
#ifndef __used
#define __used __attribute__((__used__))
#endif
#ifndef __containerof
#define __containerof(ptr, type, member) ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))
#endif
#ifndef __predict_true
#define __predict_true(x) __builtin_expect(!!(x), 1)
#define __predict_false(x) __builtin_expect(!!(x), 0)
#endif
