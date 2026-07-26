// See manual_embed.h. Reader for the manual blob linked in via EMBED_FILES.
// Format is documented in tools/pack_manual.py - keep the two in step.

#include "manual_embed.h"

#include <string.h>
#include <stdint.h>

#include "esp_log.h"

static const char *TAG = "manual";

// EMBED_FILES "manual.bin" in main/CMakeLists.txt.
extern const uint8_t manual_bin_start[] asm("_binary_manual_bin_start");
extern const uint8_t manual_bin_end[]   asm("_binary_manual_bin_end");

#define MAGIC      "QMXMAN\0\0"
#define MAGIC_LEN  8
#define HDR_LEN    (MAGIC_LEN + 4 + 4)    // magic + version + count
#define PATH_MAX_F 64
#define REC_SIZE   (PATH_MAX_F + 8)

// Read little-endian byte-wise: index records sit at 4-byte-aligned offsets, but
// doing it explicitly keeps this correct regardless of blob alignment and costs
// nothing at this scale (18 entries, scanned once per page view).
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Validate the header once and hand back the entry count. 0 = unusable.
static int blob_count(size_t *out_size)
{
    size_t size = (size_t)(manual_bin_end - manual_bin_start);
    if (out_size) *out_size = size;
    if (size < HDR_LEN) return 0;
    if (memcmp(manual_bin_start, MAGIC, MAGIC_LEN) != 0) return 0;
    if (rd32(manual_bin_start + MAGIC_LEN) != 1) return 0;          // version
    uint32_t count = rd32(manual_bin_start + MAGIC_LEN + 4);
    if (count == 0 || count > 512) return 0;
    if (HDR_LEN + (size_t)count * REC_SIZE > size) return 0;        // index fits?
    return (int)count;
}

int manual_embed_count(void)
{
    return blob_count(NULL);
}

void manual_embed_log_summary(void)
{
    size_t size = 0;
    int count = blob_count(&size);
    if (count <= 0) {
        ESP_LOGE(TAG, "built-in manual MISSING or corrupt (%u bytes) - the Reader "
                      "will have no content", (unsigned)size);
        return;
    }
    int bad = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *rec = manual_bin_start + HDR_LEN + (size_t)i * REC_SIZE;
        if ((size_t)rd32(rec + PATH_MAX_F) + rd32(rec + PATH_MAX_F + 4) > size) bad++;
    }
    if (bad) ESP_LOGE(TAG, "built-in manual: %d of %d entries out of range", bad, count);
    else     ESP_LOGI(TAG, "built-in manual: %d entries, %u KB", count, (unsigned)(size / 1024));
}

bool manual_embed_get(const char *rel, const char **out_data, size_t *out_len)
{
    if (!rel || !*rel) return false;
    size_t size = 0;
    int count = blob_count(&size);
    if (count <= 0) {
        ESP_LOGW(TAG, "embedded manual missing or corrupt (%u bytes)", (unsigned)size);
        return false;
    }

    for (int i = 0; i < count; i++) {
        const uint8_t *rec = manual_bin_start + HDR_LEN + (size_t)i * REC_SIZE;
        // The path field is NUL-padded to a fixed width, so a plain strncmp over
        // that width is both safe and an exact match test.
        if (strncmp((const char *)rec, rel, PATH_MAX_F) != 0) continue;
        uint32_t off = rd32(rec + PATH_MAX_F);
        uint32_t len = rd32(rec + PATH_MAX_F + 4);
        if ((size_t)off + len > size) {          // truncated / bad blob
            ESP_LOGW(TAG, "entry '%s' runs past the blob (off=%u len=%u size=%u)",
                     rel, (unsigned)off, (unsigned)len, (unsigned)size);
            return false;
        }
        if (out_data) *out_data = (const char *)(manual_bin_start + off);
        if (out_len)  *out_len  = len;
        return true;
    }
    return false;
}
