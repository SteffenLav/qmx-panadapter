/* Host shim - the handful of POSIX functions MinGW does not have.
 *
 * gmtime_r is the reentrant POSIX form; on a single-threaded harness gmtime()
 * is equivalent. fsync has nothing to flush through on a plain host file.
 *
 * Its own header, not part of shim_paths.h, because a harness may need these
 * WITHOUT the /spiffs path redirect - cw_grid_rows_harness builds cw_decode.c,
 * which opens no files but does call gmtime_r.
 *
 * Force-included into the firmware source only, so the device build is
 * untouched.
 */
#ifndef SHIM_MINGW_H
#define SHIM_MINGW_H
#if defined(_WIN32) && !defined(SHIM_HAVE_POSIX)
#include <time.h>
#include <unistd.h>
static inline struct tm *shim_gmtime_r(const time_t *t, struct tm *out)
{
    struct tm *g = gmtime(t);
    if (!g) return NULL;
    *out = *g;
    return out;
}
#define gmtime_r(t, o) shim_gmtime_r((t), (o))
static inline int shim_fsync(int fd) { (void)fd; return 0; }
#define fsync(fd)      shim_fsync(fd)
#endif
#endif
