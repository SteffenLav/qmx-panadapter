/* Host shim - path redirect, forced in with -include on the harness build ONLY.
 *
 * adif_log.c opens the absolute path "/spiffs/qso.adi". On the PC that is the
 * drive root, which is both wrong and rude. These macros rewrite a leading
 * "/spiffs" to the harness's scratch directory, so the REAL file code runs
 * unmodified against a real file.
 *
 * ⛔ The firmware source is NOT changed for the test. Making FILE_PATH a build
 * option so the harness could set it would put a test-only knob in shipping
 * code and would stop the harness exercising the path handling as written.
 */
#ifndef SHIM_PATHS_H
#define SHIM_PATHS_H
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

/* Set by the harness before adif_log_init(). */
void shim_set_spiffs_dir(const char *dir);
const char *shim_path(const char *p);   /* returns a rotating static buffer */

#include "shim_mingw.h"


#define fopen(p, m)   fopen(shim_path(p), (m))
#define remove(p)     remove(shim_path(p))
#define rename(a, b)  rename(shim_path(a), shim_path(b))
#define stat(p, s)    stat(shim_path(p), (s))
#define opendir(p)    opendir(shim_path(p))

#endif
