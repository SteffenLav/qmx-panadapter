#ifndef SHIM_SD_ARCHIVE_H
#define SHIM_SD_ARCHIVE_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
void sd_archive_mark_adif_dirty(void);
char *sd_archive_read_adif_file(bool previous, size_t *out_len);
/* The mirror lock. On target it serialises the card against the web server;
 * on the host there is no card and no second thread, so the shim always
 * grants it. Declared here because adif_log.c takes it around the ADIF
 * rewrite - a missing declaration here broke the harness build, not the
 * firmware, which sees the real main/storage/sd_archive.h. */
bool sd_archive_lock(uint32_t timeout_ms);
bool sd_archive_is_mounted(void);
void sd_archive_unlock(void);
#endif
