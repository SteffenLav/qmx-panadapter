#ifndef SHIM_SD_ARCHIVE_H
#define SHIM_SD_ARCHIVE_H
#include <stddef.h>
#include <stdbool.h>
void sd_archive_mark_adif_dirty(void);
char *sd_archive_read_adif_file(bool previous, size_t *out_len);
#endif
